#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <set>

namespace flockv0 {
using conversation_storage::identifier;
using conversation_storage::text;
using conversation_storage::ToolError;

namespace {
struct SavedDestination {
    Json run;
    std::string actionId, name, environmentId, projectId, threadId, runId, status, gatewayEnvironmentId;
};

SavedDestination selectSavedDestination(const Json& task, const Json& args, bool allowOmitted) {
    if (!task.contains("runResults") || !task.at("runResults").is_array())
        throw ToolError("The destination is unavailable");
    const bool hasName = args.contains("destination");
    const bool hasEnvironment = args.contains("environmentId");
    const bool hasProject = args.contains("projectId");
    const bool hasGateway = args.contains("gatewayEnvironmentId");
    if (hasName && (hasEnvironment || hasProject || hasGateway))
        throw ToolError("Select a destination either by friendly name or by environmentId and projectId, not both");
    if (hasEnvironment != hasProject)
        throw ToolError("Select a destination by both environmentId and projectId");
    if (hasGateway && !hasEnvironment)
        throw ToolError("Select gatewayEnvironmentId only with environmentId and projectId");
    const auto name = text(args,"destination",256,false);
    const auto environment = text(args,"environmentId",256,false);
    const auto project = text(args,"projectId",256,false);
    const auto gateway = text(args,"gatewayEnvironmentId",256,false);
    const auto savedGateway = [](const Json& run) {
        const auto gateway = field(run,"gatewayEnvironmentId");
        return gateway.empty() ? field(run,"environmentId") : gateway;
    };
    if ((hasName && name.empty()) || (hasEnvironment && (environment.empty() || project.empty())))
        throw ToolError("Destination selectors must not be empty");
    if (hasGateway && gateway.empty()) throw ToolError("gatewayEnvironmentId must not be empty");

    const Json* selected = nullptr;
    if (!hasName && !hasEnvironment) {
        if (!allowOmitted) throw ToolError("Select a destination by friendly name or environmentId and projectId");
        if (task.at("runResults").size() != 1)
            throw ToolError("Choose a destination by friendly name or environmentId and projectId");
        selected = &task.at("runResults").front();
    } else {
        std::vector<const Json*> matches;
        for (const auto& run : task.at("runResults")) {
            const bool match = hasName ? run.value("name","") == name
                : run.value("environmentId","") == environment && run.value("projectId","") == project &&
                    (!hasGateway || savedGateway(run) == gateway);
            if (match) matches.push_back(&run);
        }
        if (matches.size() != 1)
            throw ToolError(matches.empty() ? "The destination is unavailable" : "The destination selector is ambiguous");
        selected = matches.front();
    }
    return {*selected,field(*selected,"actionId"),field(*selected,"name"),field(*selected,"environmentId"),
            field(*selected,"projectId"),field(*selected,"threadId"),field(*selected,"runId"),field(*selected,"status"),
            field(*selected,"gatewayEnvironmentId")};
}

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
    const auto& [bot,target,title,delivery,notify] = receipt;
    if (!acceptedSessionSend(target,delivery)) return;
    exec("INSERT OR IGNORE INTO session_message_waits(message_id,bot,environment,thread,run,title,notified,gateway_environment)"
         " VALUES(?,?,?,?,?,?,?,?)",
         {message,bot,target.environmentId,target.threadId,delivery.run.runId,title,
          !notify || sessionParticipationStopped(bot,target,message) ? 1 : 0,target.gatewayEnvironmentId});
}

Store::Impl::CallOutcome Store::Impl::sendSessionMessage(
    const conversation::Message& message, int step, const Decision& decision, std::string_view key,
    const ConversationCallContext& ctx) {
    using enum CallOutcome;
    const auto id = message.id;
    const bool fixesPullRequest = decision.tool == "fix_pull_request";
    const auto save = [&](Json result) {
        saveTurnResults(id,step,Json::array({redactor(message.bot).record(std::move(result))}));
    };
    const auto saveReceipt = [&](const SessionSendReceipt& saved) {
        const auto& target = saved.target;
        const auto& receipt = saved.delivery;
        if (acceptedSessionSend(target,receipt)) {
            Json result{{"ok",true},{"delivery",receipt.delivery}};
            if (saved.notify) result.update({{"environmentId",target.environmentId},{"threadId",target.threadId}});
            save(std::move(result));
        } else {
            auto result = toolFailure(receipt.detail.empty() ? "Could not confirm delivery to T3" : receipt.detail);
            if (!receipt.definitelyUnsent && (receipt.kind == Delivery::Kind::Unknown || receipt.kind == Delivery::Kind::Accepted))
                result["delivery"] = "unknown";
            save(std::move(result));
        }
    };
    Command command;
    command.tool = "message_session";
    command.actionId = "flock-turn-" + std::to_string(id) + "-" + std::to_string(step);
    command.ownerBotId = message.bot;
    const bool savedTaskTarget = decision.tool == "send_to_task";
    Id savedTaskId = 0, savedGeneration = 0;
    std::string savedActionId;
    std::optional<GitPrFollowup> followup;
    SessionTarget target;
    Resource resource{Action::t3_send,{}};
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
                saveReceipt(receipt->second);
            } else save({{"ok",false},{"delivery","unknown"},{"error","Flock restarted while sending this. Check the Session before sending again."}});
            transaction.commit();
            sessionSendReceipts.erase(id);
            return advance;
        }
        if (ctx.hasWebEvidence && !allowedAfterWebEvidence(decision)) {
            save(webEvidenceActionDenied());
            transaction.commit();
            return advance;
        }
        try {
            if (fixesPullRequest) {
                followup = gitPrFollowup(message,ctx.intake);
                if (!followup || followup->status != GitPrFollowup::Status::available)
                    throw ToolError("Automatic follow-up is unavailable for this pull request");
                command.environmentId = followup->session.environmentId;
                command.gatewayEnvironmentId = followup->session.gatewayEnvironmentId;
                command.target.threadId = followup->session.threadId;
                command.target.gatewayEnvironmentId = followup->session.gatewayEnvironmentId;
                command.instruction = gitPrFollowupInstruction(message,*followup,decision);
            } else if (savedTaskTarget) {
                const auto task = ownTask(conversation_storage::identifier(decision.args,"taskId",true),message);
                if (conversation_storage::identifier(decision.args,"generation",true,0) != task.at("generation"))
                    throw ToolError("stale",{{"taskId",task.at("id")},{"current",taskState(task,message)}});
                if (terminal(task) || task.at("state") == "cancelling")
                    throw ToolError("The task is " + field(task,"state"),{{"taskId",task.at("id")}});
                const auto selected = selectSavedDestination(task,decision.args,true);
                command.environmentId = selected.environmentId;
                command.gatewayEnvironmentId = selected.gatewayEnvironmentId;
                command.target.threadId = selected.threadId;
                command.target.gatewayEnvironmentId = selected.gatewayEnvironmentId;
                if (command.environmentId.empty() || command.target.threadId.empty()) throw ToolError("The destination Session is unavailable");
                savedTaskId = task.at("id");
                savedGeneration = task.at("generation");
                savedActionId = selected.actionId;
                command.instruction = conversation_storage::text(decision.args,"instruction",conversation_storage::messageSize,true);
            } else {
                command.environmentId = conversation_storage::text(decision.args,"environmentId",256,true);
                command.target.threadId = conversation_storage::text(decision.args,"threadId",256,true);
                command.instruction = conversation_storage::text(decision.args,"text",conversation_storage::messageSize,true);
            }
            const auto name = redactor(message.bot).text(message.routing.at("config").value("botDisplayName",message.bot));
            command.instruction = name + ": " + command.instruction;
            if (decision.tool == "message_session" &&
                (decision.args.contains("taskId") || decision.args.contains("generation") || decision.args.contains("runId")))
                throw ToolError("Use the Session identifiers");
            target = {command.environmentId,command.target.threadId,command.gatewayEnvironmentId};
            resource = {Action::t3_send,Resource::sessionRead(target.environmentId,target.threadId).target};
            if (sessionParticipationStopped(message.bot,target,id)) throw ToolError(stopped);
            if (Policy(*this).forbidden(message.bot,resource.action,resource.target))
                throw ToolError(std::string(approvalError(ApprovalGate::denied)));
            if (!fixesPullRequest && !savedTaskTarget && !connectedSessionAccess(message,target))
                throw ToolError("This Session is unavailable through your T3 connection");
        } catch (const ToolError& failure) {
            save(toolFailure(failure.what()));
            transaction.commit();
            return advance;
        }
        transaction.commit();
    }
    std::string error;
    std::string title;
    try {
        command.authGeneration = ctx.t3.authGeneration(
            target.gatewayEnvironmentId.empty() ? target.environmentId : target.gatewayEnvironmentId,command.config);
        if (!fixesPullRequest && !savedTaskTarget) {
            if (const auto metadata = ctx.t3.sessionMetadata(target,false)) title = metadata->title;
            else error = "This Session is unavailable through your T3 connection";
        }
    } catch (const std::exception&) { error = "Could not verify access to this T3 Session"; }
    if (error.empty() && followup && followup->target.contains("headSha") &&
        (!ctx.gitHost || !currentGitPrCheck(*ctx.gitHost,message)))
        error = "Could not confirm that the failing check belongs to this pull request's current head";
    const SessionDispatchFileLock dispatch(path,SessionDispatchFileLock::Mode::shared);
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (ctx.shutdown.stop_requested()) return stop;
        if (!authorizeTurn(message)) { transaction.commit(); return stop; }
        if (sessionParticipationStopped(message.bot,target,id)) {
            save(toolFailure(stopped));
            transaction.commit();
            return advance;
        }
        if (!error.empty() || (!fixesPullRequest && !savedTaskTarget && !connectedSessionAccess(message,target))) {
            save(toolFailure(error.empty() ? "This Session is unavailable through your T3 connection" : error));
            transaction.commit();
            return advance;
        }
        auto origin = policyOrigin(message);
        Json policyPayload{decision.tool,decision.args,command.authGeneration};
        if (fixesPullRequest) {
            const auto current = gitPrFollowup(message,ctx.intake);
            if (!current || current->operation != followup->operation || current->session != target ||
                current->status != GitPrFollowup::Status::available) {
                save(toolFailure("Automatic follow-up is unavailable for this pull request"));
                transaction.commit();
                return advance;
            }
            origin = current->origin;
            policyPayload = {{"pullRequest",current->target.at("url")},{"diagnosis",decision.args.at("diagnosis")},
                {"arguments",Json::array({command.instruction,command.authGeneration,target.gatewayEnvironmentId})}};
        } else if (savedTaskTarget) {
            try {
                const auto task = ownTask(savedTaskId,message);
                if (task.at("generation") != savedGeneration)
                    throw ToolError("stale",{{"taskId",savedTaskId},{"current",taskState(task,message)}});
                if (terminal(task) || task.at("state") == "cancelling")
                    throw ToolError("The task is " + field(task,"state"),{{"taskId",savedTaskId}});
                const auto selected = selectSavedDestination(task,decision.args,true);
                if (selected.actionId != savedActionId || selected.environmentId != target.environmentId ||
                    selected.threadId != target.threadId || selected.runId.empty())
                    throw ToolError("The destination Session is unavailable");
                const auto action = outbox(task,selected.actionId);
                if (action.is_null() || !action.value("destinationAction",false) ||
                    action.value("phase","") != "accepted" || action.value("stopRequested",false) ||
                    field(action,"environmentId") != selected.environmentId ||
                    field(action,"projectId") != selected.projectId ||
                    field(action,"gatewayEnvironmentId") != selected.gatewayEnvironmentId)
                    throw ToolError("The destination Session is unavailable");
                if (!query("SELECT 1 FROM runs WHERE action_id=? AND task_id=? AND environment_id=? AND thread_id=? AND run_id=?",
                            {selected.actionId,savedTaskId,selected.environmentId,selected.threadId,selected.runId}).next())
                    throw ToolError("The destination Session is unavailable");
                origin.task = savedTaskId;
                origin.generation = savedGeneration;
                origin.revision = task.value("currentRevision",Id{});
                policyPayload.push_back(selected.actionId);
            } catch (const ToolError& failure) {
                save(toolFailure(failure.what()));
                transaction.commit();
                return advance;
            }
        }
        const auto gate = approveAction(origin,resource,key,policyPayload,policyClock());
        if (gate != ApprovalGate::allow) {
            if (gate != ApprovalGate::waiting) save(toolFailure(approvalError(gate)));
            transaction.commit();
            return gate == ApprovalGate::waiting ? waiting : advance;
        }
        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=? "
             "AND result IS NULL AND effect IS NULL",{Json{{"kind","message_session"}},id,step});
        if (!sqlite3_changes(db)) return stop;
        if (followup) recordGitPrFollowup(message,*followup);
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
            : ctx.t3.send(command);
    } catch (const std::exception&) {
        receipt.detail = "Could not confirm delivery to T3";
    }
    std::lock_guard lock(mutex);
    // If SQLite is busy, retry the receipt write without repeating the T3 send.
    const auto checkpoint = sessionSendReceipts.insert_or_assign(
        id,SessionSendReceipt{message.bot,target,std::move(title),std::move(receipt),!fixesPullRequest}).first;
    Transaction transaction(db);
    recordSessionSendRun(id,checkpoint->second);
    saveReceipt(checkpoint->second);
    transaction.commit();
    sessionSendReceipts.erase(checkpoint);
    return advance;
}

Store::Impl::CallOutcome Store::Impl::cancelDestinationRun(
    const conversation::Message& message, int step, const Decision& decision, std::string_view key,
    bool offered, bool hasWebEvidence, std::stop_token shutdown) {
    using enum CallOutcome;
    if (shutdown.stop_requested()) return stop;
    std::optional<SessionDispatchFileLock> dispatch;
    try { dispatch.emplace(path,SessionDispatchFileLock::Mode::exclusive); }
    catch (const std::exception& error) {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (authorizeTurn(message))
            saveTurnResults(message.id,step,Json::array({toolFailure(error.what())}));
        transaction.commit();
        return advance;
    }
    std::lock_guard lock(mutex);
    Transaction transaction(db);
    if (!authorizeTurn(message)) { transaction.commit(); return stop; }
    const auto save = [&](Json result) {
        saveTurnResults(message.id,step,Json::array({redactor(message.bot).record(std::move(result))}));
    };
    const auto fail = [&](const ToolError& error) {
        auto result = error.details;
        result["ok"] = false;
        result["error"] = error.what();
        save(std::move(result));
    };
    auto existing = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",
                          {message.id,step});
    if (!existing.next()) { transaction.commit(); return stop; }
    if (!existing.string(0).empty()) {
        save({{"ok",true},{"effect","stop_requested"},{"delivery","unknown"},
              {"error","The saved stop request is still being reconciled in T3"}});
        transaction.commit();
        return advance;
    }
    if (!offered) save(toolFailure("The cancellation action was not offered"));
    else if (hasWebEvidence && !allowedAfterWebEvidence(decision)) save(webEvidenceActionDenied());
    else try {
        if (!decision.args.is_object()) throw ToolError("Tool arguments must be an object");
        const auto taskId = identifier(decision.args,"taskId",true);
        const auto generation = identifier(decision.args,"generation",true,0);
        const auto task = ownTask(taskId,message);
        if (generation != task.at("generation"))
            throw ToolError("stale",{{"taskId",taskId},{"current",taskState(task,message)}});
        const auto selected = selectSavedDestination(task,decision.args,false);
        const auto action = outbox(task,selected.actionId);
        if (action.is_null() || !action.value("destinationAction",false) ||
            field(action,"environmentId") != selected.environmentId || field(action,"projectId") != selected.projectId)
            throw ToolError("The destination is unavailable");
        if (!destinationRunOutstanding(selected.run)) {
            save({{"ok",true},{"effect","already_settled"},{"taskId",taskId},{"destination",selected.name}});
        } else if (task.at("state") == "cancelling") {
            throw ToolError("The whole task is already stopping",{{"taskId",taskId}});
        } else if (terminal(task)) {
            throw ToolError("The task is " + field(task,"state"),{{"taskId",taskId}});
        } else if (action.value("stopRequested",false)) {
            save({{"ok",true},{"effect","already_requested"},{"taskId",taskId},{"destination",selected.name}});
        } else {
            const auto phase = field(action,"phase");
            if (phase != "ready" && phase != "started" && phase != "accepted")
                throw ToolError("The destination is unavailable");
            const bool exactRun = phase == "accepted" && !selected.threadId.empty() && !selected.runId.empty() &&
                query("SELECT 1 FROM runs WHERE action_id=? AND task_id=? AND environment_id=? AND thread_id=? AND run_id=?",
                      {selected.actionId,taskId,selected.environmentId,selected.threadId,selected.runId}).next();
            if (phase == "accepted" && !exactRun) throw ToolError("The destination Session is unavailable");
            if (exactRun) {
                const Resource resource{Action::t3_send,
                    Resource::sessionRead(selected.environmentId,selected.threadId).target};
                auto origin = policyOrigin(message);
                origin.task = taskId;
                origin.generation = generation;
                origin.revision = task.value("currentRevision",Id{});
                const auto gate = approveAction(origin,resource,key,
                    Json::array({decision.tool,decision.args,runJson(runRef(selected.run))}),policyClock());
                if (gate != ApprovalGate::allow) {
                    if (gate != ApprovalGate::waiting) fail(ToolError(std::string(approvalError(gate))));
                    transaction.commit();
                    return gate == ApprovalGate::waiting ? waiting : advance;
                }
            }
            auto updated = action;
            updated["stopRequested"] = true;
            updated["stopOperation"] = key;
            updated.erase("phase");
            updated.erase("authGeneration");
            exec("UPDATE outbox SET data=? WHERE id=? AND task_id=? AND phase=?",
                 Json::array({updated,selected.actionId,taskId,phase}));
            if (!sqlite3_changes(db)) throw ToolError("The destination changed before its stop request was saved");
            exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=? AND result IS NULL AND effect IS NULL",
                 {Json{{"kind","destination_cancel"},{"taskId",taskId},{"generation",generation},
                       {"actionId",selected.actionId},{"environmentId",selected.environmentId},
                       {"projectId",selected.projectId},{"run",runJson(runRef(selected.run))}},message.id,step});
            if (!sqlite3_changes(db)) throw ToolError("The stop request could not be saved");
            save({{"ok",true},{"effect","stop_requested"},{"taskId",taskId},{"destination",selected.name}});
        }
    } catch (const ToolError& error) { fail(error); }
    transaction.commit();
    return advance;
}

void Store::Impl::wakeSessionMessages(T3& t3, std::stop_token shutdown) {
    struct Wait { Id key, message; std::string bot; SessionTarget target; std::string run, title; };
    std::vector<Wait> waits;
    {
        std::lock_guard lock(mutex);
        auto rows = query("SELECT rowid,message_id,bot,environment,thread,run,title,gateway_environment "
                          "FROM session_message_waits WHERE notified=0 ORDER BY rowid");
        while (rows.next())
            waits.push_back({rows.number(0),rows.number(1),rows.string(2),
                {rows.string(3),rows.string(4),rows.string(7)},rows.string(5),rows.string(6)});
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
        const auto reason = t3.sessionMessageWake(
            wait.target,{wait.target.threadId,wait.run,{},wait.target.gatewayEnvironmentId});
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
    if (!destination.gatewayEnvironmentId.empty() && destination.gatewayEnvironmentId != destination.environmentId)
        task["gatewayEnvironmentId"] = destination.gatewayEnvironmentId;
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
    if (selectedSession && !destination.gatewayEnvironmentId.empty() &&
        destination.gatewayEnvironmentId != destination.environmentId)
        task["selectedSession"]["gatewayEnvironmentId"] = destination.gatewayEnvironmentId;
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
        const auto currentEnvironment = pendingRoute ? pendingRoute->route.environmentId : field(task,"environmentId");
        const auto currentProject = pendingRoute ? pendingRoute->route.projectId : field(task,"projectId");
        const auto currentGateway = pendingRoute ? pendingRoute->route.gatewayEnvironmentId :
            (field(task,"gatewayEnvironmentId").empty() ? currentEnvironment : field(task,"gatewayEnvironmentId"));
        const auto gateway = change->route.gatewayEnvironmentId.empty() ? environment :
            change->route.gatewayEnvironmentId;
        const bool destinationChanged = !environment.empty() && (environment != currentEnvironment ||
            (!change->route.projectId.empty() && change->route.projectId != currentProject) || gateway != currentGateway);
        std::optional<PendingDestination> nextRoute;
        if (destinationChanged) {
            auto project = change->route.projectId;
            const bool pinned = !project.empty();
            if (!pinned) project = rememberedT3Project(field(task,"botId"),environment);
            if (recipientRouteWouldCycle(task.at("id"),environment,project)) {
                if (pinned) throw Conflict("This destination would make project queues wait for one another");
                project.clear();
            }
            auto route = change->route;
            route.projectId = std::move(project);
            route.gatewayEnvironmentId = gateway;
            nextRoute = PendingDestination{std::move(route),pinned};
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
