#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <set>

namespace flockv0 {
using conversation_storage::identifier;
using conversation_storage::ToolError;

namespace {
struct SessionEffect {
    Id taskId, generation;
    Json run;
    Command command;

    Json checkpoint() const {
        Json effect = {{"kind","interrupt"},{"taskId",taskId},
                       {"environmentId",command.environmentId},{"ownerBotId",command.ownerBotId},
                       {"projectId",command.projectId},{"config",command.config},{"run",run},{"actionId",command.actionId}};
        return effect;
    }

    Json dispatch(T3& t3) const {
        const auto outcome = t3.interrupt(command.environmentId,command.config,command.target);
        if (outcome == InterruptOutcome::Requested)
            return {{"ok",true},{"effect","interrupt_requested"},{"taskId",taskId}};
        if (outcome == InterruptOutcome::AlreadyTerminal)
            return {{"ok",false},{"taskId",taskId},{"outcome","already_terminal"},
                    {"error","This work already ended"}};
        return {{"ok",false},{"taskId",taskId},{"outcome","no_active_run"},
                {"error","There is no work in progress to stop for this request"}};
    }
};
}

Store::Impl::CallOutcome Store::Impl::runSessionEffect(
    const conversation::Message& message, int step, const Decision& decision, std::string_view key,
    bool offered, bool hasWebEvidence, T3& t3, std::stop_token shutdown) {
    using enum CallOutcome;
    const auto id = message.id;
    const auto fail = [&](const ToolError& error) {
        auto failure = error.details;
        failure["ok"] = false;
        failure["error"] = error.what();
        saveTurnResults(id,step,Json::array({redactor(message.bot).record(std::move(failure))}));
    };
    const auto& args = decision.args;
    const auto admit = [&](Id taskId, Id expected, const Json& expectedRun) {
        const auto task = ownTask(taskId,message);
        const auto& run = task.at("activeRun");
        const auto state = field(task,"state");
        if (expected != task.at("generation") || (!expectedRun.is_null() && run != expectedRun))
            throw ToolError("stale",{{"applied",0},{"taskId",taskId},{"expected",{{"generation",expected}}},
                                     {"current",taskState(task,message)}});
        if (terminal(task) || state == "cancelling")
            throw ToolError("The task is " + state,{{"taskId",taskId}});
        if (task.value("replanning",false) || task.value("runStop",Json(nullptr)).is_object())
            throw ToolError("The task is stopping or replanning",{{"taskId",taskId},{"current",taskState(task,message)}});
        if (task.contains("runTargets") || task.contains("runResults"))
            throw ToolError("Session actions are not available for tasks with destination Runs; use cancel_task to stop the whole task",
                            {{"taskId",taskId}});
        if (field(run,"threadId").empty() || field(run,"runId").empty())
            throw ToolError("The task has no running Session. Use send_to_task to pass it on at the next safe point",
                            {{"taskId",taskId},{"current",taskState(task,message)}});
        return task;
    };
    std::optional<SessionEffect> effect;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested()) return stop;
        if (!authorizeTurn(message)) { transaction.commit(); return stop; }
        auto saved = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? "
                           "AND result IS NULL AND effect IS NOT NULL",{id,step});
        if (saved.next()) {
            saveTurnResults(id,step,Json::array({Json{{"ok",false},{"taskId",saved.json(0).at("taskId")},
                {"delivery","unknown"},{"error","Flock restarted while sending this, so T3 may have received it. "
                                               "Check the Session before sending it again"}}}));
        } else if (!offered) {
            saveTurnResults(id,step,Json::array({toolFailure("Session control was not offered for this turn")}));
        } else if (hasWebEvidence && !allowedAfterWebEvidence(decision)) {
            saveTurnResults(id,step,Json::array({webEvidenceActionDenied()}));
        } else {
            try {
                if (!args.is_object()) throw ToolError("Tool arguments must be an object");
                const auto taskId = identifier(args,"taskId",true);
                const auto expected = identifier(args,"generation",true,0);
                const auto task = admit(taskId,expected,nullptr);
                const auto& run = task.at("activeRun");
                Command command;
                command.actionId = "flock-turn-" + std::to_string(id) + "-" + std::to_string(step);
                command.ownerBotId = message.bot;
                command.environmentId = run.at("environmentId");
                command.projectId = run.value("projectId",field(task,"projectId"));
                command.config = task.at("config");
                command.target = runRef(run);
                effect.emplace(SessionEffect{taskId,expected,run,std::move(command)});
            } catch (const ToolError& error) { fail(error); }
        }
        transaction.commit();
    }
    if (!effect) return advance;

    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested()) return stop;
        if (!authorizeTurn(message)) { transaction.commit(); return stop; }
        try { admit(effect->taskId,effect->generation,effect->run); }
        catch (const ToolError& error) {
            fail(error);
            transaction.commit();
            return advance;
        }
        const Resource resource{Action::t3_send,
            Resource::sessionRead(effect->command.environmentId,effect->command.target.threadId).target};
        auto origin = policyOrigin(message);
        origin.task = effect->taskId;
        origin.generation = effect->generation;
        origin.revision = ownTask(effect->taskId,message).value("currentRevision",Id{});
        const auto gate = approveAction(origin,resource,key,
                                       Json::array({decision.tool,args,effect->run}),policyClock());
        if (gate != ApprovalGate::allow) {
            if (gate != ApprovalGate::waiting) fail(ToolError(std::string(approvalError(gate))));
            transaction.commit();
            return gate == ApprovalGate::waiting ? waiting : advance;
        }
        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=? "
             "AND result IS NULL AND effect IS NULL",{effect->checkpoint(),id,step});
        if (!sqlite3_changes(db)) return stop;
        transaction.commit();
    }

    Json outcome;
    try {
        const auto& cmd = effect->command;
        const auto target = Resource::sessionRead(cmd.environmentId,cmd.target.threadId).target;
        outcome = Policy(*this).forbidden(message.bot,Action::t3_send,target)
            ? toolFailure(approvalError(ApprovalGate::denied)) : effect->dispatch(t3);
    }
    catch (const std::exception& error) {
        const std::string detail = "T3 may have received the interrupt: ";
        outcome = {{"ok",false},{"taskId",effect->taskId},{"delivery","unknown"},{"error",detail + error.what()}};
    }
    outcome = redactor(message.bot).record(std::move(outcome));
    std::lock_guard lock(mutex);
    Transaction transaction(db);
    if (!query("SELECT 1 FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",{id,step}).next())
        return stop;
    if (outcome.value("ok",false)) {
        Json entry = {{"messageId",id},{"step",step},{"run",outcome.value("run",effect->run)}};
        journal(effect->taskId,"interrupt_requested",entry);
    }
    saveTurnResults(id,step,Json::array({outcome}));
    transaction.commit();
    return advance;
}

namespace {
bool acceptedSessionSend(const SessionTarget& target, const Delivery& receipt) {
    return receipt.kind == Delivery::Kind::Accepted && receipt.run.threadId == target.threadId &&
           !receipt.run.runId.empty() && (receipt.delivery == "started" || receipt.delivery == "queued");
}
}

void Store::Impl::recordSessionSendRun(Id message, const SessionSendReceipt& receipt) {
    const auto& [bot,target,title,delivery] = receipt;
    if (!acceptedSessionSend(target,delivery)) return;
    exec("INSERT OR IGNORE INTO session_message_waits(message_id,bot,environment,thread,run,title,notified) VALUES(?,?,?,?,?,?,?)",
         {message,bot,target.environmentId,target.threadId,delivery.run.runId,title,
          sessionParticipationStopped(bot,target,message) ? 1 : 0});
}

Store::Impl::CallOutcome Store::Impl::sendSessionMessage(
    const conversation::Message& message, int step, const Decision& decision, std::string_view key,
    bool hasWebEvidence, T3& t3, std::stop_token shutdown) {
    using enum CallOutcome;
    const auto id = message.id;
    const auto save = [&](Json result) {
        saveTurnResults(id,step,Json::array({redactor(message.bot).record(std::move(result))}));
    };
    Command command;
    command.tool = "message_session";
    command.actionId = "flock-turn-" + std::to_string(id) + "-" + std::to_string(step);
    command.ownerBotId = message.bot;
    std::string error;
    try {
        command.environmentId = conversation_storage::text(decision.args,"environmentId",256,true);
        command.target.threadId = conversation_storage::text(decision.args,"threadId",256,true);
        const auto name = redactor(message.bot).text(message.routing.at("config").value("botDisplayName",message.bot));
        command.instruction = name + ": " +
            conversation_storage::text(decision.args,"text",conversation_storage::messageSize,true);
        if (decision.args.contains("taskId") || decision.args.contains("generation") || decision.args.contains("runId"))
            throw ToolError("Use the Session identifiers");
    } catch (const ToolError& failure) { error = failure.what(); }
    const SessionTarget target{command.environmentId,command.target.threadId};
    const Resource resource{Action::t3_send,Resource::sessionRead(target.environmentId,target.threadId).target};
    const auto saveReceipt = [&](const Delivery& receipt) {
        if (acceptedSessionSend(target,receipt)) {
            save({{"ok",true},{"delivery",receipt.delivery},{"environmentId",target.environmentId},{"threadId",target.threadId}});
        } else {
            auto result = toolFailure(receipt.detail.empty() ? "Could not confirm delivery to T3" : receipt.detail);
            if (!receipt.definitelyUnsent && (receipt.kind == Delivery::Kind::Unknown || receipt.kind == Delivery::Kind::Accepted))
                result["delivery"] = "unknown";
            save(std::move(result));
        }
    };
    constexpr auto stopped = "This Bot's participation in the Session has stopped";
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (!authorizeTurn(message)) { transaction.commit(); return stop; }
        auto row = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",{id,step});
        if (!row.next()) return stop;
        if (!row.string(0).empty()) {
            if (const auto receipt = sessionSendReceipts.find(id); receipt != sessionSendReceipts.end()) {
                recordSessionSendRun(id,receipt->second);
                saveReceipt(receipt->second.delivery);
            } else save({{"ok",false},{"delivery","unknown"},{"error","Flock restarted while sending this. Check the Session before sending again."}});
        } else if (sessionParticipationStopped(message.bot,target,id)) {
            save(toolFailure(stopped));
        } else if (hasWebEvidence && !allowedAfterWebEvidence(decision)) {
            save(webEvidenceActionDenied());
        } else if (!error.empty()) {
            save(toolFailure(error));
        } else if (Policy(*this).forbidden(message.bot,resource.action,resource.target)) {
            save(toolFailure(approvalError(ApprovalGate::denied)));
        } else if (!connectedSessionAccess(message,target)) {
            save(toolFailure("This Session is unavailable through your T3 connection"));
        }
        auto result = query("SELECT 1 FROM conversation_steps WHERE message_id=? AND step=? AND result IS NOT NULL",{id,step});
        const bool done = result.next();
        transaction.commit();
        if (done) {
            sessionSendReceipts.erase(id);
            return advance;
        }
    }
    std::string title;
    try {
        command.authGeneration = t3.authGeneration(target.environmentId,command.config);
        if (const auto metadata = t3.sessionMetadata(target,false)) title = metadata->title;
        else error = "This Session is unavailable through your T3 connection";
    } catch (const std::exception&) { error = "Could not verify access to this T3 Session"; }
    const SessionDispatchFileLock dispatch(path,SessionDispatchFileLock::Mode::shared);
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested()) return stop;
        if (!authorizeTurn(message)) { transaction.commit(); return stop; }
        if (sessionParticipationStopped(message.bot,target,id)) {
            save(toolFailure(stopped));
            transaction.commit();
            return advance;
        }
        if (!error.empty() || !connectedSessionAccess(message,target)) {
            save(toolFailure(error.empty() ? "This Session is unavailable through your T3 connection" : error));
            transaction.commit();
            return advance;
        }
        const auto gate = approveAction(policyOrigin(message),resource,
            key,
            Json::array({decision.tool,decision.args,command.authGeneration}),policyClock());
        if (gate != ApprovalGate::allow) {
            if (gate != ApprovalGate::waiting) save(toolFailure(approvalError(gate)));
            transaction.commit();
            return gate == ApprovalGate::waiting ? waiting : advance;
        }
        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=? "
             "AND result IS NULL AND effect IS NULL",{Json{{"kind","message_session"}},id,step});
        if (!sqlite3_changes(db)) return stop;
        const auto read = Resource::sessionRead(target.environmentId,target.threadId);
        if (!message.channelScope.empty() || !Policy(*this).allows(message.bot,read)) title.clear();
        title = bounded(redactT3Tokens(redactor(message.bot).text(title)),512);
        exec(R"SQL(
INSERT INTO session_participations(bot_id,environment_id,thread_id,person_id,state)
SELECT ?,?,?,person_id,'active' FROM t3_connector WHERE singleton=1
ON CONFLICT(bot_id,environment_id,thread_id) DO NOTHING
)SQL",{message.bot,target.environmentId,target.threadId});
        transaction.commit();
    }

    Delivery receipt;
    try {
        receipt = Policy(*this).forbidden(message.bot,resource.action,resource.target)
            ? Delivery{Delivery::Kind::NotAccepted,{},std::string(approvalError(ApprovalGate::denied)),true}
            : t3.send(command);
    } catch (const std::exception&) {
        receipt.detail = "Could not confirm delivery to T3";
    }
    std::lock_guard lock(mutex);
    // If SQLite is busy, retry the receipt write without repeating the T3 send.
    const auto checkpoint = sessionSendReceipts.insert_or_assign(
        id,SessionSendReceipt{message.bot,target,std::move(title),std::move(receipt)}).first;
    Transaction transaction(db);
    recordSessionSendRun(id,checkpoint->second);
    saveReceipt(checkpoint->second.delivery);
    transaction.commit();
    sessionSendReceipts.erase(checkpoint);
    return advance;
}

void Store::Impl::wakeSessionMessages(T3& t3, std::stop_token shutdown) {
    struct Wait { Id key, message; std::string bot; SessionTarget target; std::string run, title; };
    std::vector<Wait> waits;
    {
        std::lock_guard lock(mutex);
        auto rows = query("SELECT rowid,message_id,bot,environment,thread,run,title "
                          "FROM session_message_waits WHERE notified=0 ORDER BY rowid");
        while (rows.next())
            waits.push_back({rows.number(0),rows.number(1),rows.string(2),{rows.string(3),rows.string(4)},rows.string(5),rows.string(6)});
    }
    for (const auto& wait : waits) {
        if (shutdown.stop_requested()) return;
        const auto authorized = [&] {
            const auto message = loadTurnMessage(wait.message);
            return !sessionParticipationStopped(wait.bot,wait.target,wait.message) &&
                (message.principalSource != conversation::PrincipalSource::session || activeSessionOrigin(message)) &&
                connectedSessionAccess(message,wait.target);
        };
        // A revoked connection settles the wait silently, including on restart.
        const auto settle = [&] {
            exec("UPDATE session_message_waits SET notified=1 WHERE rowid=? AND notified=0",{wait.key});
        };
        {
            std::lock_guard lock(mutex);
            if (!authorized()) {
                settle();
                continue;
            }
        }
        const auto reason = t3.sessionMessageWake(wait.target,{wait.target.threadId,wait.run,{}});
        if (reason.empty() || shutdown.stop_requested()) continue;
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (!authorized()) {
            settle();
            transaction.commit();
            continue;
        }
        settle();
        if (!sqlite3_changes(db)) continue;
        const auto source = loadTurnMessage(wait.message);
        auto routing = source.routing;
        routing.erase("sessionInvitation");
        Policy policy(*this);
        auto title = wait.title;
        if (!policy.allows(wait.bot,Resource::sessionRead(wait.target.environmentId,wait.target.threadId))) title.clear();
        if (auto origin = routing.find("sessionOrigin"); origin != routing.end() &&
            !policy.allows(wait.bot,Resource::sessionRead(field(*origin,"environmentId"),field(*origin,"threadId"))))
            (*origin)["title"] = "";
        routing["sessionWake"] = {{"environmentId",wait.target.environmentId},{"threadId",wait.target.threadId},
                                  {"title",title},{"reason",reason},{"sourceMessageId",wait.message}};
        exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'conversation_message',?,?)",
             {unixTime(),Json{{"sessionWake",routing.at("sessionWake")}},wait.bot});
        const auto id = last();
        exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing,person_id,person_root,principal_source)
SELECT ?,?,?,coalesce(max(sequence),0)+1,?,'',?,?,?,? FROM messages WHERE bot_id=?
)SQL",{id,wait.bot,"session-wake:" + std::to_string(wait.key),wait.message,routing,
       source.admittedActor.id,source.admittedActor.adminRoot ? 1 : 0,static_cast<int>(source.principalSource),wait.bot});
        exec(R"SQL(
INSERT INTO channel_messages(message_id,event_key,scope_key,origin,sender_id,person_id)
SELECT ?,?,scope_key,origin,sender_id,person_id FROM channel_messages WHERE message_id=?
)SQL",{id,"session-wake:" + std::to_string(wait.key),wait.message});
        exec("INSERT INTO conversation_turns(message_id,status) VALUES(?,'running')",{id});
        transaction.commit();
    }
}

Id Store::Impl::startConversationTask(const Json& routing, std::string_view bot, std::string_view request,
                                      const TaskRoute& destination, Id messageId, const Json& source,
                                      bool resume, const WorkSession* selectedSession) {
    if (selectedSession) redactor(bot).requirePublic(selectedSession->threadId);
    const auto id = submit({std::string(bot),std::string(request),destination.environmentId,destination.projectId,
                           routing.at("config")},messageId,source);
    auto task = load(id);
    if (routing.contains("sessionOrigin")) task["sessionOrigin"] = routing.at("sessionOrigin");
    else if (resume) {
        auto parent = query(R"SQL(
SELECT json_extract(t.data,'$.sessionOrigin') FROM tasks t
JOIN messages m ON m.id=json_extract(t.data,'$.sourceMessageId')
JOIN messages request ON request.id=? AND request.person_id=m.person_id
LEFT JOIN channel_messages channel ON channel.message_id=request.id
WHERE t.id=? AND t.bot_id=? AND json_type(t.data,'$.sessionOrigin')='object' AND channel.message_id IS NULL
)SQL",{messageId,source.at("taskId"),bot});
        if (parent.next()) task["sessionOrigin"] = parent.json(0);
    }
    if (task.contains("sessionOrigin"))
        task["sessionOrigin"]["title"] = redactor(bot).text(field(task.at("sessionOrigin"),"title"));
    if (selectedSession)
        task["selectedSession"] = {{"environmentId",destination.environmentId},{"projectId",destination.projectId},
                                   {"threadId",selectedSession->threadId}};
    // Establish provenance at intake, while both the original message and the routed request are available.
    auto message = query("SELECT text FROM messages WHERE id=? AND bot_id=?", {messageId,bot});
    task["sourceMessageCoversFullRequest"] = source.is_null() && message.next() && message.string(0) == request;
    if (resume) {
        const auto priorId = source.at("taskId").get<Id>();
        const auto prior = load(priorId);
        const auto criteria = effectiveDoneWhen(prior);
        if (!criteria.empty()) task["requestedDoneWhen"] = criteria;
        const auto pending = prior.value("cancelledInstructions",Json::array());
        std::set<std::pair<Id,int>> pendingIds;
        for (const auto& item : pending)
            pendingIds.emplace(item.at("messageId").get<Id>(),item.at("step").get<int>());

        // Excerpt only earlier deliveries. Pending directions follow them in acceptance order.
        auto rows = query("SELECT data FROM journal WHERE task_id=? AND kind='instruction_accepted'"
                          " ORDER BY id DESC",{priorId});
        Json delivered = Json::array();
        std::size_t remaining = 8000;
        while (remaining && delivered.size() < 20 && rows.next()) {
            const auto item = rows.json(0);
            if (pendingIds.contains({item.at("messageId").get<Id>(),item.at("step").get<int>()})) continue;
            const auto text = bounded(field(item,"text"),remaining);
            remaining -= text.size();
            delivered.push_back({{"messageId",item.at("messageId")},{"step",item.at("step")},{"text",text}});
        }
        std::reverse(delivered.begin(),delivered.end());
        task["intent"] = taskIntent(prior);
        task["restartContext"] = {{"taskId",priorId},{"deliveredInstructions",std::move(delivered)}};
        for (const auto& item : pending) {
            Json entry = {{"messageId",item.at("messageId")},{"step",item.at("step")},{"text",item.at("text")},
                          {"revisionId",nullptr},{"conflicting",false}};
            if (item.contains("source")) {
                const auto& source = item.at("source");
                entry["source"] = source;
                auto& refs = task["sourceResultRefs"];
                if (std::ranges::none_of(refs,[&](const Json& ref) { return ref.at("messageId") == source.at("messageId"); }))
                    refs.push_back(source);
            }
            task["pendingInstructions"].push_back(entry);
            journal(id,"instruction_accepted",entry);
        }
    }
    save(task);
    return id;
}

bool Store::Impl::acceptInstruction(Json& task, Id messageId, int step, std::string_view instruction,
                                    const Json& source, const TaskChange* change) {
    Id revision = task.value("currentRevision",Id{});
    bool conflicting = task.value("replanning",false);
    if (change) {
        const auto oldIntent = field(task,"intent");
        const auto oldDone = effectiveDoneWhen(task);
        const auto intent = change->intent.empty() ? oldIntent : change->intent;
        const auto done = change->doneWhen.empty() ? oldDone : change->doneWhen;
        const auto& environment = change->route.environmentId;
        const auto pendingRoute = PendingDestination::read(task);
        const auto currentEnvironment = pendingRoute ? pendingRoute->environmentId : field(task,"environmentId");
        const auto currentProject = pendingRoute ? pendingRoute->projectId : field(task,"projectId");
        const bool destinationChanged = !environment.empty() && (environment != currentEnvironment ||
            (!change->route.projectId.empty() && change->route.projectId != currentProject));
        std::optional<PendingDestination> nextRoute;
        if (destinationChanged) {
            auto project = change->route.projectId;
            const bool pinned = !project.empty();
            if (!pinned) project = rememberedT3Project(field(task,"botId"),environment);
            if (recipientRouteWouldCycle(task.at("id"),environment,project)) {
                if (pinned) throw Conflict("This destination would make project queues wait for one another");
                project.clear();
            }
            nextRoute = PendingDestination{environment,std::move(project),pinned};
        }
        const bool criteriaChanged = oldDone != done;
        const bool intentChanged = oldIntent != intent;
        if (criteriaChanged || intentChanged) {
            if (task.contains("currentRevision")) revision = addRevision(task,intent,done,messageId);
            else {
                task["intent"] = intent;
                if (criteriaChanged) {
                    task["requestedDoneWhen"] = done;
                    task["criteriaSourceMessageId"] = messageId;
                }
                journal(task.at("id"),"task_contract_changed",{{"messageId",messageId}});
            }
            if (intentChanged) task["sourceMessageCoversFullRequest"] = false;
            conflicting = true;
        }
        if (nextRoute) {
            nextRoute->write(task);
            conflicting = true;
        }
    }
    Json pending = {{"messageId",messageId},{"step",step},{"text",instruction},{"revisionId",revision},
                    {"conflicting",conflicting}};
    if (!source.is_null()) {
        pending["source"] = source;
        auto& references = task["sourceResultRefs"];
        if (references.is_null()) references = Json::array();
        if (std::ranges::none_of(references,[&](const Json& ref) { return ref.at("messageId") == source.at("messageId"); }))
            references.push_back(source);
    }
    if (conflicting) {
        task["replanning"] = true;
        releaseModelCall(takeReservedTaskCall(task.value("eventId",Id{0})));
        if (task.at("state") == "waiting" && waitIsActionable(task)) {
            auto data = task.at("wait");
            if (!data.is_object()) throw DecisionUnavailable{};
            for (const auto* key : {"id","messageId","target"}) data.erase(key);
            const auto reason = field(data,"reason");
            clearWait(task,"request_changed");
            wait(task,reason,std::move(data));
            if (task["question"].is_object()) task["question"]["target"] = questionTarget(task);
        }
    }
    if (!task.contains("pendingInstructions")) task["pendingInstructions"] = Json::array();
    task["pendingInstructions"].push_back(pending);
    if (task["question"].is_object()) task["question"].erase("suspendedDecision");
    journal(task.at("id"),"instruction_accepted",pending);
    save(task);
    return conflicting;
}

void Store::Impl::recordTaskBlocker(Json& task, std::string_view reason, Id waitId) {
    if ((!reason.empty() && !conversation_storage::hasText(reason)) || reason.size() > 2000)
        throw Conflict("Provide a blocker reason of up to 2,000 bytes, or an empty string to clear it");
    const auto& wait = task.at("wait");
    if (waitId && (reason.empty() || task.at("state") != "waiting" || wait.is_null() || wait.at("id") != waitId ||
                   !waitIsActionable(task))) throw DecisionUnavailable{};
    const auto publicReason = redactor(field(task,"botId")).text(std::string(reason));
    task.erase("blockerWaitId");
    if (reason.empty()) task.erase("blockerReason");
    else task["blockerReason"] = publicReason;
    if (waitId) task["blockerWaitId"] = waitId;
    auto entry = Json{{"blockerReason",publicReason},{"targetRevision",task.value("currentRevision",Id{0})}};
    if (waitId) entry["waitId"] = waitId;
    journal(task.at("id"),reason.empty() ? "blocker_cleared" : "blocker_recorded",entry);
    save(task);
}

void Store::Impl::withdrawTaskQuestion(Json& task, Id waitId) {
    const auto& wait = task.at("wait");
    if (task.at("state") != "waiting" || field(wait,"reason") != "user_question" ||
        wait.at("id") != waitId || !waitIsActionable(task))
        throw Conflict("The question is no longer open");
    if (wait.contains("projectChoices")) throw Conflict("Choose a T3 project before this task can start");
    auto updated = currentEvent(task);
    for (const auto* metadata : {"id","failures","retryAt","evidence","consumed"}) updated.erase(metadata);
    updated["withdrawnUserQuestion"] = field(wait,"text");
    consumeEvent(task);
    event(task,"withdraw/" + std::to_string(waitId),updated);
    clearWait(task,"withdrawn");
    if (task["question"].is_object()) task["question"]["target"] = questionTarget(task);
    journal(task.at("id"),"question_withdrawn",{{"waitId",waitId}});
    task["state"] = "deciding";
    save(task);
}

}
