#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <set>

namespace flockv0 {
using conversation_storage::verifiedEnvironment;
using conversation_storage::identifier;
using conversation_storage::ToolError;

namespace {
enum class SessionControl { Steer, Interrupt };

struct SessionEffect {
    SessionControl control;
    Id taskId, generation;
    Json run;
    Command command;

    Json checkpoint() const {
        Json effect = {{"kind",control == SessionControl::Steer ? "steer" : "interrupt"},{"taskId",taskId},
                       {"environmentId",command.environmentId},{"ownerBotId",command.ownerBotId},
                       {"projectId",command.projectId},{"config",command.config},{"run",run},{"actionId",command.actionId}};
        if (control == SessionControl::Steer) effect["text"] = command.instruction;
        return effect;
    }

    Json dispatch(T3& t3) const {
        if (control == SessionControl::Interrupt) {
            const auto outcome = t3.interrupt(command.environmentId,command.config,command.target);
            if (outcome == InterruptOutcome::Requested)
                return {{"ok",true},{"effect","interrupt_requested"},{"taskId",taskId}};
            if (outcome == InterruptOutcome::AlreadyTerminal)
                return {{"ok",false},{"taskId",taskId},{"outcome","already_terminal"},
                        {"error","The assigned Run already ended"}};
            return {{"ok",false},{"taskId",taskId},{"outcome","no_active_run"},
                    {"error","T3 has no active Run to interrupt"}};
        }
        const auto delivery = t3.send(command);
        if (delivery.kind == Delivery::Kind::Accepted)
            return {{"ok",true},{"effect","delivered"},{"taskId",taskId},{"run",runJson(delivery.run)}};
        Json failed = {{"ok",false},{"taskId",taskId},
            {"error",delivery.detail.empty() ? "T3 did not accept the note" : delivery.detail}};
        if (delivery.kind == Delivery::Kind::Unknown && !delivery.definitelyUnsent) failed["delivery"] = "unknown";
        return failed;
    }
};
}

bool Store::Impl::runSessionEffect(const conversation::Message& message, int step, const Decision& decision,
                                   bool offered, bool hasWebEvidence, T3& t3, std::stop_token shutdown) {
    const auto id = message.id;
    const auto fail = [&](const ToolError& error) {
        auto failure = error.details;
        failure["ok"] = false;
        failure["error"] = error.what();
        saveTurnResults(id,step,Json::array({redactor(message.bot).record(std::move(failure))}));
    };
    const auto& args = decision.args;
    const auto control = decision.tool == "message_session" ? SessionControl::Steer : SessionControl::Interrupt;
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
        if (field(run,"threadId").empty() || field(run,"runId").empty())
            throw ToolError("The task has no running Session. Use send_to_task to pass it on at the next safe point",
                            {{"taskId",taskId},{"current",taskState(task,message)}});
        return task;
    };
    std::optional<SessionEffect> effect;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested()) return false;
        if (!authorizeTurn(message)) { transaction.commit(); return false; }
        auto saved = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? "
                           "AND result IS NULL AND effect IS NOT NULL",{id,step});
        if (saved.next()) {
            saveTurnResults(id,step,Json::array({Json{{"ok",false},{"taskId",saved.json(0).at("taskId")},
                {"delivery","unknown"},{"error","Flock restarted while sending this, so T3 may have received it. "
                                               "Check the Session before sending it again"}}}));
        } else if (control == SessionControl::Steer && !t3.supportsRunTargetedSteer()) {
            saveTurnResults(id,step,Json::array({toolFailure("This T3 connection cannot target a specific running Run. "
                "Use send_to_task to save the instruction for the task's next safe point.")}));
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
                command.environmentId = field(run,"environmentId");
                if (command.environmentId.empty()) command.environmentId = field(task,"environmentId");
                command.projectId = field(task,"projectId");
                command.config = task.at("config");
                command.target = runRef(run);
                if (control == SessionControl::Steer) {
                    command.tool = "steer";
                    command.instruction = conversation_storage::text(args,"text",conversation_storage::messageSize,true);
                }
                effect.emplace(SessionEffect{control,taskId,expected,run,std::move(command)});
            } catch (const ToolError& error) { fail(error); }
        }
        transaction.commit();
    }
    if (!effect) return true;

    if (control == SessionControl::Steer) {
        auto& command = effect->command;
        try { command.authGeneration = t3.authGeneration(command.environmentId,command.config); }
        catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            Transaction transaction(db);
            if (!authorizeTurn(message)) { transaction.commit(); return false; }
            fail(ToolError(error.what(),{{"taskId",effect->taskId}}));
            transaction.commit();
            return true;
        }
    }
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested()) return false;
        if (!authorizeTurn(message)) { transaction.commit(); return false; }
        try { admit(effect->taskId,effect->generation,effect->run); }
        catch (const ToolError& error) {
            fail(error);
            transaction.commit();
            return true;
        }
        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=? "
             "AND result IS NULL AND effect IS NULL",{effect->checkpoint(),id,step});
        if (!sqlite3_changes(db)) return false;
        transaction.commit();
    }

    Json outcome;
    try { outcome = effect->dispatch(t3); }
    catch (const std::exception& error) {
        const std::string detail = control == SessionControl::Steer ? "T3 may have received the note: "
                                                                   : "T3 may have received the interrupt: ";
        outcome = {{"ok",false},{"taskId",effect->taskId},{"delivery","unknown"},{"error",detail + error.what()}};
    }
    outcome = redactor(message.bot).record(std::move(outcome));
    std::lock_guard lock(mutex);
    Transaction transaction(db);
    if (!query("SELECT 1 FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",{id,step}).next())
        return false;
    if (outcome.value("ok",false)) {
        Json entry = {{"messageId",id},{"step",step},{"run",outcome.value("run",effect->run)}};
        if (control == SessionControl::Steer) {
            entry["text"] = effect->command.instruction;
            entry["revisionId"] = effect->run.value("revisionId",Json(nullptr));
        }
        journal(effect->taskId,control == SessionControl::Steer ? "instruction_steered" : "interrupt_requested",entry);
    }
    saveTurnResults(id,step,Json::array({outcome}));
    transaction.commit();
    return true;
}

Id Store::Impl::startConversationTask(const Json& routing, std::string_view bot, std::string_view request,
                                      std::string_view environment, Id messageId, const Json& source,
                                      bool resume) {
    const auto destination = environment.empty() ? field(routing,"environmentId")
        : verifiedEnvironment(routing,routing,environment);
    const auto id = submit({std::string(bot),std::string(request),destination,field(routing,"projectId"),
                            routing.at("config")},messageId,source);
    auto task = load(id);
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
                                    const Json& source, const Json& routing, const TaskChange* change) {
    Id revision = task.value("currentRevision",Id{});
    bool conflicting = task.value("replanning",false);
    if (change) {
        const auto oldIntent = field(task,"intent");
        const auto oldDone = effectiveDoneWhen(task);
        const auto intent = change->intent.empty() ? oldIntent : change->intent;
        const auto done = change->doneWhen.empty() ? oldDone : change->doneWhen;
        const auto environment = verifiedEnvironment(task,routing,change->environment);
        const bool destinationChanged = !environment.empty() &&
            environment != task.value("pendingEnvironmentId",field(task,"environmentId"));
        if (destinationChanged) {
            const auto id = task.at("id").get<Id>();
            const auto peer = peerExchange(id);
            if (peer && peer->recipient == id && peer->kind == "work" &&
                peerWorkRouteWouldCycle(peer->sender,environment,field(task,"projectId")))
                throw Conflict("This destination would make project queues wait for one another");
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
        if (destinationChanged) {
            task["pendingEnvironmentId"] = environment;
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
