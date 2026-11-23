#include "storage.hpp"
#include "conversation_storage.hpp"
#include "flock/bot_profile.hpp"
#include "flock/participation.hpp"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <fcntl.h>
#include <future>
#include <map>
#include <set>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

namespace flockv0 {
namespace {
bool present(std::string_view value, std::size_t limit = 8000) {
    return value.size() <= limit && value.find_first_not_of(" \r\n\t") != std::string_view::npos;
}
bool hasRun(const RunRef& r) { return !r.threadId.empty() && !r.runId.empty(); }

Json revision(const Json& task) { return task.value("currentRevision",Json(nullptr)); }

bool hasPending(const Json& task) {
    const auto pending = task.find("pendingInstructions");
    return pending != task.end() && !pending->empty();
}

struct SourceQuotes {
    const SecretRedactor& scrub;
    std::size_t remaining = 8192;
    std::set<Id> included{};

    std::string quote(const Json& source, std::size_t limit = 8192) {
        const auto id = source.at("messageId").get<Id>();
        if (!id || !included.insert(id).second) return {};
        auto text = "\n\nReferenced result " + std::to_string(id) + " from task " +
            std::to_string(source.at("taskId").get<Id>());
        if (source.contains("stageId")) text += "\nStage: " + field(source,"stageId");
        if (source.contains("run")) text += "\nSource Run: " + source.at("run").dump();
        if (source.contains("resultRunId")) text += "\nReport Run: " + field(source,"resultRunId");
        text += " (reference only):";
        const auto excerpt = bounded(scrub.text(field(source,"text")),std::min(remaining,limit));
        remaining -= excerpt.size();
        if (!excerpt.empty()) text += "\n" + excerpt;
        return scrub.text(text);
    }
};

Json pendingBatch(const Json& task, std::size_t maxPromptBytes, SourceQuotes quotes) {
    Json batch = Json::array();
    std::size_t bytes = 0, promptBytes = 0;
    if (!hasPending(task)) return batch;
    for (const auto& item : task.at("pendingInstructions")) {
        bytes += item.dump().size();
        promptBytes += 2 + field(item,"text").size();
        if (item.contains("source")) promptBytes += quotes.quote(item.at("source")).size();
        if (!batch.empty() && (bytes > 64000 || promptBytes > maxPromptBytes)) break;
        batch.push_back(item);
    }
    return batch;
}

struct TaskDecision : Decision {
    Json usage = Json::object();
    Id attemptId = 0;
    std::string rejection{};
};

struct TaskInference {
    Id generation, eventId, attemptId;
    Json revisionId;
    Json usage = Json::object();
    std::variant<ModelCall,ModelError> outcome{};
};

std::string gitInvocation(const TaskDecision& decision) {
    return "attempt/" + std::to_string(decision.attemptId);
}

struct SessionContextLimit : Conflict {
    SessionContextLimit() : Conflict("The Session prompt exceeds T3's size limit. "
        "Continue the existing Session if possible, or revise the task before starting a new Session.") {}
};

bool needsDoneWhen(const Json& task) {
    return !task.contains("currentRevision") && field(task,"requestedDoneWhen").empty();
}

void includeInstructions(std::string& instruction, const Json& pending) {
    if (pending.empty()) return;
    instruction += "\n\nUser instructions accepted for this task, oldest first. Apply later directions where they replace earlier ones:";
    for (const auto& item : pending) instruction += "\n\n" + field(item,"text");
}
}

struct Runtime::Impl {
    Store::Impl& db;
    Policy policy;
    Model& model;
    T3& t3;
    SoulLoader soul;
    RuntimeOptions options;
    std::future<void> channelWorker, sessionWakeWorker;
    std::stop_source shutdown;
    int lockFd = -1;
    // An empty action denotes a Task step. Verification Runs are scheduled by their own outbox action.
    using Work = std::pair<Id,std::string>;
    std::map<Work,std::future<void>> workers;
    // Keep provider outcomes until the Task decision and attempt usage are committed together.
    std::map<Id,TaskInference> taskInferences;
    struct HistoryUnavailable : ModelError { using ModelError::ModelError; };
    std::map<std::string,std::future<void>> conversationWorkers;

    void settleApprovals() {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        db.settlePolicyApprovals(options.clock());
        tx.commit();
    }

    void fireDueAutomations() {
        try { db.fireDueAutomations(options.clock()); }
        catch (const StorageError&) { throw; }
        catch (const std::exception& error) {
            if (!options.automationError) throw;
            options.automationError(error.what());
        }
    }

    void deferContendedWork(std::string_view operation, const auto& work) {
        try { work(); }
        catch (const StorageError& error) {
            if (!error.contended()) throw;
            if (options.storageContention)
                options.storageContention(std::string(operation) + ": " + error.what());
        }
    }

    void runTurn(Id id) {
        db.runTurn(id,model,t3,soul,options.clock,*options.botTools,options.gitHost.get(),options.applyBotChange,
                   options.reconcileBotCreation,options.intake,options.peers,shutdown.get_token());
    }

    Impl(Store& store, Model& m, T3& adapter, SoulLoader loader, RuntimeOptions opts)
        : db(*store.p_), policy(store.policy()), model(m), t3(adapter),
          soul(std::move(loader)), options(std::move(opts)) {
        if (!options.botTools) options.botTools = std::make_shared<BotTools>();
        const auto lockPath = std::filesystem::canonical(db.path).string() + ".runtime.lock";
        lockFd = open(lockPath.c_str(), O_CLOEXEC | O_CREAT | O_RDWR, 0600);
        if (lockFd < 0 || ::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
            if (lockFd >= 0) close(lockFd);
            throw Conflict("Cannot acquire the exclusive Runtime lock for this database");
        }
        try { db.recoverWorkspace(); }
        catch (...) { close(lockFd); throw; }
        {
            std::lock_guard lock(db.mutex);
            db.policyClock = options.clock;
        }
    }
    ~Impl() {
        shutdown.request_stop();
        conversationWorkers.clear();
        workers.clear();
        if (channelWorker.valid()) channelWorker.wait();
        if (sessionWakeWorker.valid()) sessionWakeWorker.wait();
        try { persistTaskInferences(); }
        catch (const std::exception& error) {
            std::fprintf(stderr,"Flock Task inference shutdown: %s\n",error.what());
        }
        try { db.stopWorkspace(); }
        catch (const std::exception& error) {
            std::fprintf(stderr,"Flock workspace shutdown: %s\n",error.what());
        }
        {
            std::lock_guard lock(db.mutex);
            db.policyClock = unixTime;
        }
        close(lockFd);
    }

    void saveTask(Json& task, Json resumedDecision = nullptr) {
        task.erase("pendingDecision");
        if (!resumedDecision.is_null()) {
            resumedDecision["generation"] = task.at("generation").get<Id>() + 1;
            task["pendingDecision"] = std::move(resumedDecision);
        }
        db.save(task);
    }

    void consumeEvent(Json& task) {
        task.erase("feedback");
        db.consumeEvent(task);
    }

    template<class F> bool changeTask(Id id, F fn, Json* saved = nullptr) {
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        auto task = db.load(id);
        if (!fn(task)) return false;
        saveTask(task);
        transaction.commit();
        if (saved) *saved = std::move(task);
        return true;
    }

    template<class F> bool change(const Json& snapshot, F fn, Json* saved = nullptr) {
        return changeTask(snapshot["id"],[&](Json& task) {
            return task["generation"] == snapshot["generation"] && task["eventId"] == snapshot["eventId"] &&
                   revision(task) == revision(snapshot) && fn(task);
        },saved);
    }

    std::vector<Work> eligible() {
        std::lock_guard lock(db.mutex);
        auto rows = db.query(R"SQL(
WITH runnable AS (
  SELECT id FROM project_slots WHERE blocker_id IS NULL
  UNION
  SELECT id FROM tasks WHERE project_id='' AND state NOT IN ('completed','cancelled') AND (
    state IN ('queued','cancelling') OR COALESCE(json_extract(data,'$.replanning'),0)=1
  )
  UNION
  SELECT id FROM tasks WHERE state='queued' AND json_extract(data,'$.replanning')=1
    AND json_extract(data,'$.pendingDestination.environmentId')<>environment_id
  UNION
  SELECT tasks.id FROM tasks JOIN peer_exchanges ON tasks.id=recipient_task
  WHERE kind='consult' AND state NOT IN ('completed','cancelled')
  UNION
  SELECT tasks.id FROM tasks JOIN peer_exchanges ON tasks.id=recipient_task
  WHERE state='cancelling' AND json_extract(tasks.data,'$.activeRun') IS NULL AND NOT EXISTS (
    SELECT 1 FROM outbox WHERE outbox.id=json_extract(tasks.data,'$.actionId') AND phase='started'
  )
)
SELECT task.id,'' AS action_id FROM runnable JOIN tasks task ON task.id=runnable.id
WHERE json_type(task.data,'$.runResults') IS NULL AND NOT EXISTS (
  SELECT 1 FROM policy_approvals approval WHERE approval.task=task.id AND turn=0 AND status='pending'
)
UNION ALL
SELECT slot.id,slot.action_id FROM project_route_slots slot JOIN tasks task ON task.id=slot.id
WHERE slot.action_id IS NOT NULL AND (task.state='cancelling' OR (
  slot.blocker_id IS NULL AND NOT EXISTS (
    SELECT 1 FROM policy_approvals WHERE operation=?1 || slot.action_id AND status='pending'
  )
))
ORDER BY 1,2)SQL",{runOutboxOperation});
        std::vector<Work> result;
        while (rows.next()) result.emplace_back(rows.number(0),rows.string(1));
        return result;
    }
    static constexpr unsigned batchLimit = 4;
    Work lastScheduled;

    std::vector<Work> nextProjects(std::size_t slots) {
        if (slots == 0) return {};
        auto jobs = eligible();
        std::erase_if(jobs,[&](const Work& work) { return workers.contains(work); });
        if (jobs.size() > slots) {
            std::rotate(jobs.begin(),std::upper_bound(jobs.begin(),jobs.end(),lastScheduled),jobs.end());
            jobs.resize(slots);
        }
        if (!jobs.empty()) lastScheduled = jobs.back();
        return jobs;
    }

    Json snapshot(Id id) {
        std::lock_guard lock(db.mutex);
        return db.load(id);
    }

    bool selectProject(Json& task) {
        const auto bot = field(task,"botId");
        const auto environment = field(task,"environmentId");
        {
            std::lock_guard lock(db.mutex);
            if (db.projectQuestionOpen(bot,environment)) return false;
        }
        auto catalog = db.readProjectCatalog(t3,bot,environment,options.requireT3Connection);
        return change(task,[&](Json& current) {
            return !shutdown.stop_requested() && db.applyProjectCatalog(current,std::move(catalog));
        },&task) && !field(task,"projectId").empty();
    }

    void activate(Json task) {
        if (field(task,"projectId").empty() && !selectProject(task)) return;
        change(task,[&](Json& current) {
            auto turn = db.query("SELECT 1 FROM project_slots WHERE id=? AND blocker_id IS NULL",{current["id"]});
            if (!turn.next()) return false;
            if (current.contains("workflowId") && current["lastRun"].is_null()) {
                queueAction(current,actionId(),"delegate","",Json::object(),Json::object(),false);
                return true;
            }
            if (current["eventId"] == 0) db.event(current,"intake",{{"kind","intake"}});
            current["state"] = "deciding";
            return true;
        });
    }

    void refundUnsentWork(Json& task, const Json& action) {
        if (action.at("chargesBudget").get<bool>()) {
            task["remaining"] = task["remaining"].get<int>() + 1;
            task["additionalUsed"] = task["additionalUsed"].get<int>() - 1;
        }
    }

    void cancelDone(Json& task) {
        closeQuestion(task,"cancelled");
        if (!field(task,"actionId").empty()) {
            const auto action = db.outbox(task);
            db.exec("UPDATE outbox SET phase='closed' WHERE id=? AND phase='ready'", {task["actionId"]});
            if (sqlite3_changes(db.db)) refundUnsentWork(task,action);
            task["actionId"] = "";
        }
        task.erase("runStop");
        task.erase("replanning");
        task.erase("pendingDestination");
        task.erase("queuedInT3");
        if (hasPending(task)) task["cancelledInstructions"] = task.at("pendingInstructions");
        else task.erase("cancelledInstructions");
        task.erase("pendingInstructions");
        task["activeRun"] = nullptr;
        task["state"] = "cancelled";
        db.journal(task["id"],"cancelled",Json::object());
        if (const auto peer = db.peerExchange(task["id"]); peer && peer->recipient == task["id"])
            finishPeer(*peer,task,db.load(peer->sender),"cancelled","The receiving Bot's task was cancelled.",actionId());
    }

    void stopNote(const Json& snapshot, const std::string& detail) {
        change(snapshot,[&](Json& task) {
            const bool cancel = task["state"] == "cancelling";
            const auto key = cancel ? "cancellationDetail" : "replanningDetail";
            if (field(task,key) == detail) return false;
            task[key] = detail;
            db.journal(task["id"],cancel ? "cancelling" : "replanning",{{"detail",detail},{"run",task["activeRun"]}});
            return true;
        });
    }

    void closeQuestion(Json& task, std::string_view reason, std::string_view resolution = {}) {
        db.clearWait(task,reason);
        db.unlockQuestion(task,resolution.empty() ? reason : resolution);
    }

    void suspendQuestion(Json& task) {
        auto& question = task["question"];
        if (!question.is_object()) return;
        const auto reason = field(task["wait"],"reason");
        if (reason == "user_question" || reason == "model_budget" || reason == "model_failure")
            question["suspendedWaitId"] = task["wait"].at("id");
        const auto pending = task.find("pendingDecision");
        if (pending != task.end() && pending->at("eventId") == task.at("eventId") &&
            pending->at("generation") == task.at("generation") && pending->at("revisionId") == revision(task))
            question["suspendedDecision"] = *pending;
    }

    Json resumeQuestion(Json& task) {
        auto& question = task["question"];
        const auto& target = question.at("target");
        const auto waitId = question.value("suspendedWaitId",Json(nullptr));
        db.clearWait(task,"connection_restored");
        task["state"] = "deciding";
        if (!waitId.is_null()) {
            auto row = db.query("SELECT data FROM waits WHERE id=? AND task_id=? AND resolution='superseded'",
                                {waitId,task.at("id")});
            if (row.next()) {
                auto data = row.json(0);
                const auto reason = field(data,"reason");
                if (data.at("eventId") == target.at("eventId") && data.at("run") == target.at("run") &&
                    (reason != "user_question" || data.at("questionId") == target.at("requestId")))
                    db.wait(task,reason,std::move(data));
            }
        }
        auto decision = question.value("suspendedDecision",Json(nullptr));
        question.erase("suspendedWaitId");
        question.erase("suspendedDecision");
        question["target"] = questionTarget(task);
        if (!decision.is_null() && (decision.at("revisionId") != revision(task) ||
            decision.at("eventId") != task.at("eventId"))) return nullptr;
        return decision;
    }

    void writeRunReport(Json& task, const Json& data) {
        const auto run = runRef(data.at("run"));
        if (task["state"] != "cancelling" && !task.value("replanning",false)) {
            db.event(task,"result/" + field(data,"environmentId") + "/" + run.threadId + "/" + run.runId,data);
        }
        const auto reportId = db.journal(task.at("id"),"report",data);
        if (task.contains("workflowId")) {
            task["pendingStageReport"] = reportId;
            db.settleStageReport(task);
        }
    }

    RunEvidence fetchRunEvidence(const Json& task, const Json& report) {
        auto source = runRef(report.at("run"));
        source.runId = field(report,"resultRunId");
        RunEvidence evidence;
        try {
            evidence = t3.readRunEvidence(field(report,"environmentId"),task.at("config"),source,field(report,"status"));
        } catch (const std::exception& error) {
            evidence.availability = RunEvidence::Availability::ReportUnavailable;
            evidence.detail = error.what();
        }
        const auto scrub = db.redactor(field(task,"botId"));
        evidence.report = scrub.text(evidence.report);
        evidence.detail = scrub.text(evidence.detail);
        if (scrub.text(evidence.itemId) != evidence.itemId) evidence.itemId.clear();
        return evidence;
    }

    void recoverRunReport(const Json& task) {
        const auto& pending = task.at("pendingRunReport");
        if (options.clock() < pending.at("retryAt").get<Seconds>()) return;
        auto report = pending.at("report");
        const auto evidence = fetchRunEvidence(task,report);
        setReportEvidence(report,evidence);
        change(task,[&](Json& current) {
            if (evidence.availability == RunEvidence::Availability::ReportUnavailable && pending.at("attempts") == 0) {
                current["pendingRunReport"] = {{"report",report},{"retryAt",options.clock()+10},{"attempts",1}};
            } else {
                current.erase("pendingRunReport");
                writeRunReport(current,report);
            }
            return true;
        });
    }

    bool ingest(const Json& snapshot, const Observation& observed) {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        auto task = db.load(snapshot.at("id"));
        if (task.at("generation") != snapshot.at("generation") || task.at("eventId") != snapshot.at("eventId") ||
            revision(task) != revision(snapshot) || task.at("activeRun") != snapshot.at("activeRun")) return false;
        if (task.contains("workflowId")) db.stageObservation(task,observed,options.clock());
        Json resumedDecision;
        const bool changed = [&] {
            const auto run = runRef(task["activeRun"]);
            const auto state = field(task,"state");
            const auto reason = field(task["wait"],"reason");
            const bool connectionWait = reason == "auth" || reason == "settings";
            const bool replanning = task.value("replanning",false);
            if (observed.kind == Observation::Kind::Finished) {
                closeQuestion(task,"run_finished");
                if (!field(task,"actionId").empty()) {
                    const auto action = db.outbox(task);
                    const bool supersededAnswer = !action.is_null() && action["tool"] == "answer_session_question";
                    if (supersededAnswer) {
                        db.exec("UPDATE outbox SET phase='closed' WHERE id=?", {task["actionId"]});
                        task["actionId"] = "";
                    }
                }
                task["lastRun"] = task["activeRun"];
                task["activeRun"] = nullptr;
                task.erase("queuedInT3");
                consumeEvent(task);
                Json data = {{"kind","result"},{"run",task["lastRun"]},
                    {"resultRunId",observed.resultRunId.empty() ? run.runId : observed.resultRunId},
                    {"status",observed.status},{"environmentId",task["environmentId"]},
                    {"revisionId",task.at("lastRun").at("revisionId")}};
                setReportEvidence(data,{observed.evidence,observed.report,observed.itemId,observed.detail,
                    observed.truncated,observed.sessionCreatedAt});
                db.exec("INSERT OR IGNORE INTO t3_events VALUES(?,?,'result',?)",{task["environmentId"],run.threadId,run.runId});
                if (sqlite3_changes(db.db)) {
                    if (observed.evidence == RunEvidence::Availability::ReportUnavailable && !observed.resultRunId.empty() &&
                        state != "cancelling" && !replanning) {
                        db.journal(task.at("id"),"run_settled",data);
                        task["pendingRunReport"] = {{"report",data},{"retryAt",options.clock()+5},{"attempts",0}};
                    } else writeRunReport(task,data);
                }
                if (task.contains("runStop")) {
                    db.journal(task["id"],"interrupt_settled",task.at("runStop"));
                    task.erase("runStop");
                }
                if (state == "cancelling") cancelDone(task);
                else if (!terminal(task)) task["state"] = "deciding";
                return true;
            }
            if (state == "cancelling" || replanning) return false;
            if (!field(task,"actionId").empty()) return false;
            bool leftQueue = false;
            if (task.contains("selectedSession") && observed.kind != Observation::Kind::Unavailable) {
                if (observed.kind == Observation::Kind::Running && observed.status == "queued") {
                    if (connectionWait) {
                        db.clearWait(task,"connection_restored");
                        task["state"] = "running";
                    }
                    if (task.value("queuedInT3",false)) return connectionWait;
                    task["queuedInT3"] = true;
                    task["deadline"] = 0;
                    return true;
                }
                leftQueue = task.erase("queuedInT3") != 0;
                if (leftQueue)
                    task["deadline"] = options.clock() + db.environmentTimeout(field(task,"botId"),field(task,"environmentId"),task.at("config"));
            }
            if (observed.kind == Observation::Kind::Question) {
                if (observed.question.requestId.empty()) return false;
                if (field(task["question"],"requestId") == observed.question.requestId) {
                    if (!connectionWait && state != "running") return false;
                    auto saved = task["question"].value("target",Json(nullptr));
                    if (!saved.is_object()) return false;
                    saved["waitId"] = nullptr;
                    if (saved != questionTarget(task)) return false;
                    resumedDecision = resumeQuestion(task);
                    return true;
                }
                db.exec("INSERT OR IGNORE INTO t3_events VALUES(?,?,'question',?)",
                        {task["environmentId"],run.threadId,observed.question.requestId});
                if (!sqlite3_changes(db.db)) return false;
                closeQuestion(task,"session_question","replaced");
                consumeEvent(task);
                const Json question = {{"requestId",observed.question.requestId},{"fields",observed.question.fields}};
                task["question"] = question;
                const Json data = {{"kind","question"},{"run",runJson(run)},{"question",question}};
                db.event(task,"question/" + field(task,"environmentId") + "/" + run.threadId + "/" + observed.question.requestId,data);
                task["question"]["target"] = questionTarget(task);
                db.journal(task["id"],"session_question",data);
                task["state"] = "deciding";
                return true;
            }
            if (observed.kind == Observation::Kind::ExternalInput) {
                if (reason == "t3_input") return false;
                db.unlockQuestion(task,"requires_t3");
                consumeEvent(task);
                db.wait(task,"t3_input",{{"text",bounded(observed.status,2048)}});
                return true;
            }
            if (observed.kind == Observation::Kind::Unavailable) {
                if ((observed.status == "auth" || observed.status == "settings") && observed.status != reason) {
                    suspendQuestion(task);
                    db.wait(task,observed.status,{{"text",observed.status == "auth" ?
                        "T3 authentication is unavailable" : "T3 connection settings need attention"}});
                    return true;
                }
            }
            const bool available = observed.kind != Observation::Kind::Unavailable;
            if (available && (reason == "t3_input" || !task["question"].is_null())) {
                closeQuestion(task,"session_question_resolved","resolved_in_t3");
                consumeEvent(task);
                task["state"] = "running";
                return true;
            }
            if (available && connectionWait) {
                db.clearWait(task,"connection_restored");
                task["state"] = "running";
                return true;
            }
            if (state == "running" && task["deadline"].get<Seconds>() > 0 &&
                options.clock() >= task["deadline"].get<Seconds>()) {
                db.wait(task,"deadline",{{"text","This request reached its deadline"}});
                return true;
            }
            return leftQueue;
        }();
        if (changed) saveTask(task,std::move(resumedDecision));
        tx.commit();
        return changed;
    }

    Observation observeRun(const std::string& bot, const std::string& environment, const Json& config, const RunRef& run) {
        try {
            auto observed = t3.observe(environment,config,run);
            const auto scrub = db.redactor(bot);
            scrub.requirePublic(observed.question.requestId);
            for (auto* value : {&observed.resultRunId,&observed.itemId,&observed.checkedCommit})
                if (scrub.text(*value) != *value) value->clear();
            observed.question.fields = scrub.payload(observed.question.fields);
            observed.report = scrub.text(observed.report);
            observed.detail = scrub.text(observed.detail);
            if (observed.status!="auth" && observed.status!="settings" && observed.status!="completed" && observed.status!="failed" &&
                observed.status!="cancelled" && observed.status!="interrupted" && observed.status!="rolled_back")
                observed.status = scrub.text(observed.status);
            return observed;
        } catch (const std::exception&) {
            Observation observation;
            observation.kind = Observation::Kind::Unavailable;
            return observation;
        }
    }

    Observation observe(const Json& task) {
        const auto& run = task.at("activeRun");
        return observeRun(field(task,"botId"),run.value("environmentId",field(task,"environmentId")),task.at("config"),runRef(run));
    }

    void interruptRun(const Json& task) {
        const auto& stop = task.at("runStop");
        if (task["activeRun"].is_null() || task["activeRun"] != stop.at("run") ||
            (task["state"] == "cancelling") != (stop["intent"] == "cancel")) return;
        const auto observed = observe(task);
        if (ingest(task,observed)) return;
        if (observed.kind == Observation::Kind::Settling)
            return stopNote(task,"Waiting for the remaining work on this request to finish");
        if (observed.kind == Observation::Kind::Unavailable &&
            !(task.contains("workflowId") && stop.at("intent") == "cancel"))
            return stopNote(task,"Flock couldn't check the status of this work in T3");
        const auto now = options.clock();
        if (now < stop.value("retryAt",Seconds{0})) return;
        Json requested;
        if (!change(task,[&](Json& current) {
            if (shutdown.stop_requested()) return false;
            current["runStop"]["retryAt"] = now + 5;
            return true;
        },&requested)) return;
        InterruptOutcome outcome;
        try {
            const auto& run = stop.at("run");
            outcome = t3.interrupt(run.value("environmentId",field(task,"environmentId")),task["config"],runRef(run));
        }
        catch (const std::exception&) { return stopNote(requested,"T3 did not confirm the interrupt request"); }
        if (outcome != InterruptOutcome::Requested)
            return stopNote(requested,"Waiting for T3 to confirm this work has stopped");
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        db.journal(task["id"],"interrupt_requested",stop);
        transaction.commit();
    }

    void finishReplan(const Json& task) {
        change(task,[&](Json& current) {
            const auto id = current["id"].get<Id>();
            const auto peer = db.peerExchange(id);
            if (peer && !peer->allowsReplan(id)) return false;
            const auto oldEnvironment = field(current,"environmentId");
            const auto destination = PendingDestination::read(current);
            const bool moving = destination && destination->environmentId != oldEnvironment;
            const auto event = db.currentEvent(current);
            const bool preservePeerAnswer = peer && !event.is_null() && event["consumed"] != true &&
                event["kind"] == "bot_answer";
            if (moving) {
                current["environmentId"] = destination->environmentId;
                current["projectId"] = destination->pinned ? destination->projectId : std::string{};
                current["newSessionRequired"] = true;
                current.erase("selectedSession");
                db.journal(id,"session_destination_changed",
                    {{"from",oldEnvironment},{"to",destination->environmentId},{"revisionId",revision(current)}});
            }
            if (destination && (destination->pinned || destination->projectId.empty()))
                current.erase("pendingDestination");
            closeQuestion(current,"user_instruction");
            if (!preservePeerAnswer) {
                consumeEvent(current);
                auto report = settledSessionReport(current);
                db.event(current,"replan/" + std::to_string(current["generation"].get<Id>()),
                         report.is_null() ? Json{{"kind","intake"}} : report);
            }
            const bool needsQueue = moving || current["state"] == "queued" || field(current,"projectId").empty() ||
                (current.contains("workflowId") && current["lastRun"].is_null());
            current["state"] = needsQueue ? "queued" : "deciding";
            for (const auto* key : {"replanning","replanningDetail","runStop","feedback"}) current.erase(key);
            return true;
        });
    }

    void stop(Json task) {
        const bool cancel = task["state"] == "cancelling";
        const auto intent = cancel ? "cancel" : "replan";
        if (task.contains("pendingRunReport")) {
            if (!change(task,[&](Json& current) {
                const auto report = current.at("pendingRunReport").at("report");
                current.erase("pendingRunReport");
                writeRunReport(current,report);
                return true;
            },&task)) return;
        }
        Json action;
        {
            std::lock_guard lock(db.mutex);
            action = db.outbox(task);
        }
        if (!task["activeRun"].is_null() || (!action.is_null() && action["phase"] == "started")) {
            const auto saved = task.value("runStop",Json(nullptr));
            if (!saved.is_object() || saved.at("intent") != intent || saved.at("run") != task["activeRun"]) {
                if (!change(task,[&](Json& current) {
                    if (!saved.is_object() || saved.at("run") != current["activeRun"])
                        current["runStop"] = {{"run",current["activeRun"]}};
                    current["runStop"]["intent"] = intent;
                    return true;
                },&task)) return;
            }
        }
        if (!action.is_null() && action["phase"] == "started") {
            const auto now = options.clock();
            const bool reply = action["tool"] == "answer_session_question";
            if (now >= task.at("runStop").value("reconcileAt",Seconds{0}) && change(task,[&](Json& current) {
                current["runStop"]["reconcileAt"] = now + 3;
                return true;
            })) deliver(task,action);
            if (reply && !task["activeRun"].is_null()) interruptRun(snapshot(task["id"]));
            return;
        }
        if (!cancel && !action.is_null() && action["phase"] == "ready") {
            change(task,[&](Json& current) {
                db.exec("UPDATE outbox SET phase='superseded' WHERE id=? AND phase='ready'", {action["id"]});
                if (!sqlite3_changes(db.db)) return false;
                refundUnsentWork(current,action);
                current["actionId"] = "";
                db.clearWait(current,"superseded_instruction");
                db.journal(current["id"],"delivery_superseded",{{"actionId",action["id"]},{"revisionId",revision(current)}});
                return true;
            });
            return;
        }
        if (!task["activeRun"].is_null()) interruptRun(task);
        else if (cancel) change(task,[&](Json& current) { cancelDone(current); return true; });
        else finishReplan(task);
    }

    Command command(const Json& task, const Json& action) {
        Command c;
        c.ownerBotId = field(task,"botId");
        c.actionId = field(action,"id");
        c.tool = field(action,"tool");
        c.environmentId = action.at("environmentId");
        c.projectId = action.value("projectId",field(task,"projectId"));
        c.instruction = field(action,"instruction");
        c.config = task["config"];
        if (action.contains("expectedCommit")) {
            const auto& commit = action.at("expectedCommit");
            c.config["session"]["workspaceStrategy"] = {{"type","worktree"},{"baseRef",commit},{"startFromOrigin",false}};
            c.config["commitVerification"] = {{"expectedCommit",commit},{"projectId",c.projectId}};
        }
        c.target = runRef(action.value("target",Json::object()));
        c.questionId = field(action,"questionId");
        c.answers = action.value("answers",Json::object());
        c.authGeneration = field(action,"authGeneration");
        c.candidate = runRef(action.value("candidate",Json::object()));
        c.candidateQuestionId = field(action,"candidateQuestionId");
        c.launchReceiptThreadId = field(action,"launchReceiptThreadId");
        if (c.tool == "delegate" && task.contains("sessionOrigin")) {
            const auto& origin = task.at("sessionOrigin");
            c.sessionOrigin = SessionOrigin{{field(origin,"environmentId"),field(origin,"threadId")},field(origin,"title")};
        }
        return c;
    }

    void instructionDelivery(Json& task, const Json& action) {
        const auto& instructions = action.at("instructions");
        if (instructions.empty()) return;
        auto& pending = task["pendingInstructions"];
        for (const auto& item : instructions) {
            const auto copy = std::ranges::find_if(pending,[&](const Json& p) {
                return p.at("messageId") == item.at("messageId") && p.at("step") == item.at("step");
            });
            if (copy != pending.end()) pending.erase(copy);
            db.journal(task["id"],"instruction_delivered",{{"actionId",action["id"]},{"messageId",item["messageId"]},
                {"step",item.at("step")},{"revisionId",action.at("revisionId")},{"run",task["activeRun"]},
                {"source",item.value("source",Json(nullptr))}});
        }
    }

    bool assignedRunMatches(const Json& task, const Json& action, const RunRef& run) {
        const auto& environment = action.at("environmentId");
        auto owner = db.query("SELECT action_id FROM runs WHERE environment_id=? AND thread_id=? AND run_id=?",
                              {environment,run.threadId,run.runId});
        if (owner.next() && owner.string(0) != field(action,"id")) return false;
        if (action["tool"] == "start_in_session")
            return run.threadId == field(action.at("target"),"threadId");
        if (action["tool"] == "follow_up") {
            const auto target = runRef(action["target"]);
            if (run.threadId != target.threadId || run.runId == target.runId) return false;
            auto prior = db.query("SELECT 1 FROM runs WHERE task_id=? AND environment_id=? AND thread_id=? AND run_id=?",
                                  {task["id"],environment,target.threadId,target.runId});
            return prior.next();
        }
        auto prior = db.query("SELECT 1 FROM runs WHERE environment_id=? AND thread_id=? AND action_id<>? LIMIT 1",
                              {environment,run.threadId,action["id"]});
        return !prior.next();
    }

    enum class DeliveryOutcome { Accepted, DefinitelyUnsent, Unknown };

    DeliveryOutcome classifyDelivery(const Json& task, const Json& action, Delivery& outcome, bool reply) {
        const auto scrub = db.redactor(field(task,"botId"));
        if (scrub.payload(runJson(outcome.run)) != runJson(outcome.run))
            outcome = {Delivery::Kind::Unknown,{},"T3 returned a protected Run identifier"};
        outcome.detail = scrub.text(outcome.detail);
        if (outcome.kind == Delivery::Kind::Accepted) {
            if (reply || (hasRun(outcome.run) && assignedRunMatches(task,action,outcome.run)))
                return DeliveryOutcome::Accepted;
            outcome.detail = "The returned Run does not identify the assigned work";
            outcome.run = {};
        } else if (outcome.definitelyUnsent || outcome.kind == Delivery::Kind::NotAccepted)
            return DeliveryOutcome::DefinitelyUnsent;
        return DeliveryOutcome::Unknown;
    }

    void recordRunAssignment(const Json& task, const Json& action, const RunRef& ref, Json& run) {
        db.exec("INSERT OR IGNORE INTO runs VALUES(?,?,?,?,?,?)",
                {action.at("id"),task.at("id"),action.at("environmentId"),ref.threadId,ref.runId,ref.link});
        run.update(runJson(ref));
        run["ownerBotId"] = task.at("botId");
        run["environmentId"] = action.at("environmentId");
        run["projectId"] = action.value("projectId",field(task,"projectId"));
        run["revisionId"] = action.at("revisionId");
        db.journal(task.at("id"),"run_started",{{"run",run},{"actionId",action.at("id")},
            {"revisionId",run.at("revisionId")},{"sourceResultRefs",action.at("sourceResultRefs")}});
    }

    void rememberRunCandidate(Json action, const RunRef& ref) {
        action.erase("phase");
        action.erase("authGeneration");
        action["candidate"] = runJson(ref);
        if (action.at("tool") == "delegate") action["launchReceiptThreadId"] = ref.threadId;
        db.exec("UPDATE outbox SET data=? WHERE id=?",{action,action.at("id")});
    }

    void writeCheckReport(Json& task, Json& run, const Observation& observed) {
        if (db.recordVerificationReport(task,run,observed) && task.at("state") == "cancelling")
            cancelDone(task);
    }

    static bool setCheckStatus(Json& run, std::string_view status, std::string detail) {
        if (run.at("status") == status && field(run,"detail") == detail) return false;
        run["status"] = status;
        run["detail"] = std::move(detail);
        return true;
    }

    void checkStatus(Id id, std::string_view action, std::string_view status, std::string detail) {
        changeTask(id,[&](Json& task) {
            auto& run = verificationRun(task,action);
            return verificationRunOutstanding(run) && setCheckStatus(run,status,std::move(detail));
        });
    }

    bool deferVerificationDelivery(Json& task, Json& run, const Json& action, const Delivery& outcome,
                                  DeliveryOutcome delivery) {
        const bool cancelling = task.at("state") == "cancelling";
        const auto key = action.at("id");
        const auto phase = field(action,"phase");
        const auto detail = bounded(outcome.detail.empty() ? "T3 has not confirmed the assigned Run." : outcome.detail,2048);
        // Only an explicit pre-send failure can close a delivery. Reconciliation never sends again.
        const bool rejected = outcome.kind == Delivery::Kind::NotAccepted || outcome.kind == Delivery::Kind::Settings;
        if (delivery == DeliveryOutcome::DefinitelyUnsent && (cancelling || phase == "started" || rejected)) {
            db.exec("UPDATE outbox SET phase='closed' WHERE id=?",{key});
            Observation observed;
            observed.status = cancelling ? "cancelled" : "failed";
            observed.detail = detail;
            writeCheckReport(task,run,observed);
            return true;
        }
        if (phase == "started" && !outcome.run.threadId.empty() && !action.contains("candidate")) {
            rememberRunCandidate(action,outcome.run);
            run["link"] = outcome.run.link;
            run["threadId"] = outcome.run.threadId;
        }
        setCheckStatus(run,"confirmation_pending",detail);
        run["retryAt"] = options.clock() + 5;
        return true;
    }

    void stepCheck(const Json& task, const Json& run) {
        const auto id = task.at("id").get<Id>();
        const auto key = field(run,"actionId");
        Json action;
        {
            std::lock_guard lock(db.mutex);
            action = db.outbox(task,key);
        }
        const auto phase = field(action,"phase");
        const auto now = options.clock();
        const bool cancelling = task.at("state") == "cancelling";
        if (cancelling && phase == "ready") {
            applyDelivery(id,action,{Delivery::Kind::NotAccepted,{},"Cancelled before dispatch",true});
            return;
        }
        if (now < run.value("retryAt",Seconds{})) return;
        if (phase == "ready") {
            std::lock_guard lock(db.mutex);
            if (!db.projectRouteReady(id,key)) return;
        }
        if (phase == "ready" || phase == "started") return deliver(task,std::move(action));
        if (phase != "accepted") return;
        const auto cmd = command(task,action);
        const auto ref = runRef(run);
        const auto observed = observeRun(cmd.ownerBotId,cmd.environmentId,cmd.config,ref);
        if (observed.kind == Observation::Kind::Finished) {
            changeTask(id,[&](Json& current) {
                auto& saved = verificationRun(current,key);
                if (!verificationRunOutstanding(saved)) return false;
                if (current.at("state") != "cancelling" &&
                    (observed.commitEvidenceUnavailable || observed.evidence == RunEvidence::Availability::ReportUnavailable) &&
                    saved.value("evidenceAttempts",std::size_t{}) + 1 < maxEvidenceReads) {
                    saved["evidenceAttempts"] = saved.value("evidenceAttempts",0) + 1;
                    saved["status"] = "confirmation_pending";
                    saved["detail"] = "The Run ended. Flock is retrieving its commit and report.";
                    saved["retryAt"] = now + 5;
                    return true;
                }
                writeCheckReport(current,saved,observed);
                return true;
            });
            return;
        }
        using Kind = Observation::Kind;
        changeTask(id,[&](Json& current) {
            auto& saved = verificationRun(current,key);
            if (!verificationRunOutstanding(saved) || current.at("state") == "cancelling") return false;
            bool changed = false;
            if (observed.kind == Kind::Unavailable)
                changed = setCheckStatus(saved,"confirmation_pending","Flock could not confirm the Run's status in T3.");
            else if (observed.kind == Kind::Question || observed.kind == Kind::ExternalInput)
                changed = setCheckStatus(saved,"confirmation_pending","Review the pending input or approval in this T3 Session.");
            else
                changed = setCheckStatus(saved,observed.status == "queued" ? "queued" : "running",{});
            const auto deadline = saved.value("deadline",Seconds{});
            if (current.at("wait").is_null() && deadline > 0 && now >= deadline) {
                db.wait(current,"deadline",{{"text",field(saved,"name") + " reached its deadline"},
                    {"runActionId",key},{"run",{{"threadId",ref.threadId},{"runId",ref.runId},
                                               {"link",ref.link},{"environmentId",cmd.environmentId}}}});
                return true;
            }
            return changed;
        });
        if (!cancelling || observed.kind == Kind::Settling || observed.kind == Kind::Unavailable ||
            now < run.value("stopRetryAt",Seconds{})) return;
        if (!changeTask(id,[&](Json& current) {
            if (shutdown.stop_requested() || current.at("state") != "cancelling") return false;
            verificationRun(current,key)["stopRetryAt"] = now + 5;
            return true;
        })) return;
        try { t3.interrupt(cmd.environmentId,cmd.config,ref); }
        catch (const std::exception&) {
            checkStatus(id,key,"confirmation_pending","T3 did not confirm the stop request.");
        }
    }

    void applyDelivery(Id id, const Json& action, Delivery outcome) {
        changeTask(id,[&](Json& task) {
            if (terminal(task)) return false;
            const auto key = field(action,"id");
            const bool verification = action.contains("expectedCommit");
            Json saved = verification ? db.outbox(task,key) : Json(nullptr);
            if (verification ? (saved.is_null() || (saved.at("phase") != "ready" && saved.at("phase") != "started"))
                             : task.at("actionId") != action.at("id")) return false;
            if (!outcome.launchWarning.empty()) {
                task["launchWarning"] = db.redactor(field(task,"botId")).text(outcome.launchWarning);
                db.journal(id,"session_lineage_unavailable",{{"actionId",key},{"text",task["launchWarning"]}});
            }
            const bool cancelling = task["state"] == "cancelling";
            const bool reply = action["tool"] == "answer_session_question";
            const auto delivery = classifyDelivery(task,action,outcome,reply);

            const bool accepted = delivery == DeliveryOutcome::Accepted &&
                (!verification || saved.at("phase") == "started");
            if (accepted) db.exec("UPDATE outbox SET phase='accepted' WHERE id=?",{key});
            if (verification) {
                auto& run = verificationRun(task,key);
                if (!accepted) return deferVerificationDelivery(task,run,saved,outcome,delivery);
                run["status"] = "running";
                run.erase("detail");
                run.erase("retryAt");
                recordRunAssignment(task,action,outcome.run,run);
                return true;
            }
            if (accepted) {
                db.clearWait(task,"delivery_confirmed");
                if (reply) db.unlockQuestion(task,"answered");
                else {
                    task["activeRun"] = Json::object();
                    recordRunAssignment(task,action,outcome.run,task["activeRun"]);
                    if (const auto participation = action.value("sessionParticipationId",Id{}); participation)
                        db.exec(R"SQL(
INSERT OR IGNORE INTO session_queue_cancellations(participation_id,run,stop_id)
SELECT id,?,stopped_through FROM session_participations WHERE id=? AND stopped_through>=?
)SQL",{outcome.run.runId,participation,task.at("sourceMessageId")});
                    if (task.contains("workflowId")) {
                        const auto now = options.clock();
                        db.exec("UPDATE workflow_stages SET confirmed_state='running',confirmed_at=?,unreachable=0,"
                                "status=CASE WHEN status='pending' THEN 'running' ELSE status END,"
                                "deadline=CASE WHEN deadline=0 AND json_extract(spec,'$.softDeadlineSeconds')>0 "
                                "THEN ?+json_extract(spec,'$.softDeadlineSeconds') ELSE deadline END WHERE task_id=?", {now,now,id});
                    }
                    instructionDelivery(task,action);
                    if ((action["tool"] == "delegate" || action["tool"] == "start_in_session") &&
                        !task.value("replanning",false)) {
                        task.erase("newSessionRequired");
                        task.erase("freshContextRequested");
                    }
                }
                if (reply) db.journal(id,"session_answered",{{"requestId", action["questionId"]},{"actionId",action["id"]}});
                task["actionId"] = "";
                if (!cancelling) task["state"] = "running";
                return true;
            }
            if (outcome.kind == Delivery::Kind::QuestionGone && reply) {
                db.exec("UPDATE outbox SET phase='closed' WHERE id=?", {action["id"]});
                closeQuestion(task,"question_gone","resolved_or_expired");
                task["actionId"] = "";
                if (!cancelling) task["state"] = "running";
                return true;
            }
            if (delivery == DeliveryOutcome::Unknown && !outcome.run.threadId.empty() &&
                action["phase"] == "ready" && !reply && !cancelling) {
                rememberRunCandidate(action,outcome.run);
                db.journal(id,"run_id_pending",{{"actionId",action["id"]},{"thread",runJson(outcome.run)}});
                return true;
            }
            const auto reason = outcome.kind == Delivery::Kind::Auth ? "auth" :
                                outcome.kind == Delivery::Kind::Settings ? "settings" : "unknown_delivery";
            auto detail = outcome.detail.empty() ? "The assigned side effect could not be confirmed" :
                                                   bounded(outcome.detail,2048);
            if (reply && delivery == DeliveryOutcome::DefinitelyUnsent)
                detail = bounded("The Session answer was not sent. " + detail,2048);
            if (delivery == DeliveryOutcome::DefinitelyUnsent) {
                db.exec("UPDATE outbox SET phase=? WHERE id=?", {cancelling ? "closed" : "ready",action["id"]});
                if (!reply) task["deadline"] = 0;
                if (cancelling) {
                    refundUnsentWork(task,action);
                    task["actionId"] = "";
                }
            }
            if (cancelling) {
                if (field(task,"cancellationDetail") == detail && delivery == DeliveryOutcome::Unknown) return false;
                task["cancellationDetail"] = detail;
                db.journal(id,"cancelling",{{"detail",detail},{"actionId",action["id"]}});
            } else if (field(task["wait"],"reason") != reason || field(task["wait"],"text") != detail) {
                db.wait(task,reason,{{"text",detail}});
            } else if (delivery == DeliveryOutcome::Unknown) return false;
            return true;
        });
    }

    void deliver(const Json& task, Json action) {
        if (shutdown.stop_requested()) return;
        auto cmd = command(task,action);
        const auto id = task["id"].get<Id>();
        Delivery outcome;
        const bool usesExistingSession = cmd.tool == "follow_up" || cmd.tool == "start_in_session";
        const bool sendsToSession = usesExistingSession || cmd.tool == "answer_session_question";
        const bool preparesTaskContract = cmd.tool == "delegate" || cmd.tool == "start_in_session";
        const auto sessionActionAllowed = [&](const Json& current) {
            if (!cmd.sessionOrigin && !sendsToSession) return true;
            std::lock_guard lock(db.mutex);
            if (const auto origin = current.find("sessionOrigin"); origin != current.end()) {
                const auto source = db.loadTurnMessage(current.at("sourceMessageId"));
                const auto actor = db.turnActor(source);
                const SessionTarget parent{field(*origin,"environmentId"),field(*origin,"threadId")};
                if (!db.activeSessionOrigin(cmd.ownerBotId,actor.id,parent,cmd.environmentId,source.id) ||
                    !policy.allows(actor,Action::bot_decide,cmd.ownerBotId) ||
                    !policy.allows(cmd.ownerBotId,Action::environment_inspect,parent.environmentId)) return false;
                if (!sendsToSession) return true;
            }
            if (db.sessionParticipationStopped(cmd.ownerBotId,{cmd.environmentId,cmd.target.threadId},
                                               current.value("sourceMessageId",Id{}))) return false;
            if (!usesExistingSession || !current.contains("selectedSession")) return cmd.tool != "start_in_session";
            const auto& selected = current.at("selectedSession");
            if (field(selected,"environmentId") != cmd.environmentId || field(selected,"threadId") != cmd.target.threadId ||
                field(selected,"projectId") != cmd.projectId) return false;
            return db.permitsWorkSession(current.value("sourceMessageId",Id{}),
                                         {cmd.environmentId,cmd.target.threadId,cmd.projectId});
        };
        std::optional<Resource> resource;
        if (cmd.tool == "delegate") resource = Resource{Action::environment_launch,cmd.environmentId};
        else if (sendsToSession) {
            resource = Resource{Action::t3_send,Resource::sessionRead(cmd.environmentId,cmd.target.threadId).target,
                                cmd.tool == "answer_session_question"};
        }
        const bool check = action.contains("expectedCommit");
        const auto operation = std::string(check ? runOutboxOperation : "outbox/") + cmd.actionId;
        const auto refused = [&](ApprovalGate gate) {
            if (check) return applyDelivery(id,action,{Delivery::Kind::NotAccepted,{},std::string(approvalError(gate)),true});
            change(task,[&](Json& current) {
                if (current["actionId"] != action["id"]) return false;
                db.exec("UPDATE outbox SET phase='closed' WHERE id=? AND phase='ready'",{action["id"]});
                if (!sqlite3_changes(db.db)) return false;
                refundUnsentWork(current,action);
                current["actionId"] = "";
                current["state"] = current["activeRun"].is_null() ? "deciding" : "running";
                current["feedback"] = approvalError(gate);
                if (current.at("state") == "deciding")
                    db.event(current,"policy/" + cmd.actionId,{{"kind","policy_outcome"},{"text",approvalError(gate)}});
                db.journal(id,"policy_action_closed",{{"actionId",cmd.actionId},{"text",approvalError(gate)}});
                return true;
            });
        };
        const bool firstSend = action["phase"] == "ready";
        const auto disconnected = [&] {
            return options.requireT3Connection && !db.t3EnvironmentConnected(cmd.environmentId);
        };
        const auto connectionUnavailable = [&] {
            applyDelivery(id,action,{Delivery::Kind::Auth,{},
                "Connect this T3 environment in Flock settings before sending work.",true});
        };
        std::optional<SessionDispatchFileLock> dispatch;
        if (firstSend) {
            if (disconnected()) return connectionUnavailable();
            if (!sessionActionAllowed(task)) return refused(ApprovalGate::denied);
            if (resource && policy.mode() == "full" && policy.forbidden(cmd.ownerBotId,resource->action,resource->target))
                return applyDelivery(id,action,{Delivery::Kind::Settings,{},"Session action denied by installation policy.",true});
            if (preparesTaskContract) {
                try {
                    std::lock_guard lock(db.mutex);
                    if (!action.contains("instruction")) {
                        prepareDelegation(task,action);
                        auto prepared = action;
                        prepared.erase("phase");
                        prepared.erase("authGeneration");
                        db.exec("UPDATE outbox SET data=? WHERE id=? AND phase='ready' "
                                "AND json_type(data,'$.instruction') IS NULL",{prepared,cmd.actionId});
                        auto saved = db.query("SELECT data FROM outbox WHERE id=? AND phase='ready'",{cmd.actionId});
                        if (!saved.next()) return;
                        action.update(saved.json(0));
                    }
                    cmd.instruction = field(action,"instruction");
                } catch (const Conflict& error) {
                    return applyDelivery(id,action,{Delivery::Kind::Settings,{},error.what(),true});
                }
            }
            if (preparesTaskContract || usesExistingSession) {
                const auto scrub = db.redactor(cmd.ownerBotId);
                const auto pin = field(task,"inputArtifactVersion");
                if (scrub.text(pin) != pin)
                    return applyDelivery(id,action,{Delivery::Kind::Settings,{},"The pinned workflow artifact version contains a protected value",true});
                const auto marker = sessionPrompt(cmd.actionId,{});
                if (cmd.instruction.ends_with(marker)) cmd.instruction.resize(cmd.instruction.size()-marker.size());
                cmd.instruction = sessionPrompt(cmd.actionId,scrub.text(cmd.instruction));
                if (cmd.instruction.size() > maxSessionPromptBytes)
                    return applyDelivery(id,action,{Delivery::Kind::Settings,{},SessionContextLimit{}.what(),true});
                action["instruction"] = cmd.instruction;
            }
            auto payload = Json{{"tool",cmd.tool},{"instruction",cmd.instruction},{"questionId",cmd.questionId},
                                  {"answers",cmd.answers},{"run",runJson(cmd.target)},{"projectId",cmd.projectId},
                                  {"config",cmd.config},{"environmentId",cmd.environmentId}};
            if (cmd.sessionOrigin) payload["delegatedFrom"] = task.at("sessionOrigin");
            const auto assess = [&](const Json& current, bool claim) {
                const auto origin = db.taskPolicyOrigin(current);
                if (usesExistingSession) {
                    const Resource launch{Action::environment_launch,cmd.environmentId,true};
                    const auto launchPolicy = policy.check(cmd.ownerBotId,launch);
                    if (launchPolicy == PolicyResult::deny) return ApprovalGate::denied;
                    if (launchPolicy == PolicyResult::ask &&
                        policy.check(cmd.ownerBotId,*resource) == PolicyResult::allow)
                        return db.approveAction(origin,launch,operation,payload,options.clock(),claim);
                }
                return resource ? db.approveAction(origin,*resource,operation,payload,options.clock(),claim)
                                : ApprovalGate::allow;
            };
            if (resource) {
                ApprovalGate gate;
                {
                    std::lock_guard lock(db.mutex);
                    Transaction tx(db.db);
                    const auto current = check ? db.load(id) : task;
                    if (check && current.at("state") == "cancelling") return;
                    gate = assess(current,false);
                    tx.commit();
                }
                if (gate == ApprovalGate::waiting) {
                    if (check) checkStatus(id,cmd.actionId,"confirmation_pending","Waiting for launch approval");
                    return;
                }
                if (gate != ApprovalGate::allow) return refused(gate);
            }
            std::optional<Delivery> stopped;
            try {
                stopped = t3.preflight(cmd);
                if (!stopped) cmd.authGeneration = t3.authGeneration(cmd.environmentId,cmd.config);
            } catch (const std::exception& error) {
                stopped = Delivery{Delivery::Kind::Settings,{},error.what()};
            }
            if (stopped) {
                stopped->definitelyUnsent = true;
                return applyDelivery(id,action,*stopped);
            }
            if (options.requireT3Connection || sendsToSession || cmd.sessionOrigin)
                dispatch.emplace(db.path,SessionDispatchFileLock::Mode::shared);
            if (disconnected()) return connectionUnavailable();
            auto policyGate = ApprovalGate::allow;
            const auto startSend = [&](Json& current) {
                if (shutdown.stop_requested()) return false;
                if (check) {
                    if (current.at("state") == "cancelling" || terminal(current) ||
                        !db.projectRouteReady(id,cmd.actionId)) return false;
                } else if (current["actionId"] != action["id"]) return false;
                if (!sessionActionAllowed(current)) {
                    policyGate = ApprovalGate::denied;
                    return false;
                }
                if (resource) {
                    policyGate = assess(current,true);
                    if (policyGate != ApprovalGate::allow) return false;
                }
                if (usesExistingSession && current.contains("sourceMessageId")) {
                    const auto source = db.loadTurnMessage(current.at("sourceMessageId"));
                    const SessionTarget target{cmd.environmentId,cmd.target.threadId};
                    auto participant = db.query(R"SQL(
SELECT id,person_id FROM session_participations WHERE bot_id=? AND environment_id=? AND thread_id=? AND state='active'
)SQL",{cmd.ownerBotId,target.environmentId,target.threadId});
                    if (participant.next() && (participant.string(1) == db.turnActor(source).id ||
                                               db.connectedSessionAccess(source,target)))
                        action["sessionParticipationId"] = participant.number(0);
                }
                auto payload = action;
                payload.erase("phase");
                payload.erase("authGeneration");
                db.exec("UPDATE outbox SET phase='started',auth_generation=?,data=?"
                        " WHERE id=? AND phase='ready'",{cmd.authGeneration,payload,action["id"]});
                if (!sqlite3_changes(db.db)) return false;
                auto& timed = check ? verificationRun(current,cmd.actionId) : current;
                if (cmd.tool != "answer_session_question" && timed.value("deadline",Seconds{}) == 0)
                    timed["deadline"] = options.clock() + db.environmentTimeout(cmd.ownerBotId,cmd.environmentId,current.at("config"));
                db.journal(current["id"],"send_started",{{"actionId",action["id"]}});
                return true;
            };
            if (!(check ? changeTask(id,startSend) : change(task,startSend))) {
                if (policyGate != ApprovalGate::allow && policyGate != ApprovalGate::waiting) refused(policyGate);
                return;
            }
        } else if (action["phase"] != "started") return;
        try {
            if (firstSend) outcome = !sessionActionAllowed(task) ||
                                     (resource && policy.forbidden(cmd.ownerBotId,resource->action,resource->target)) ||
                                     (usesExistingSession && policy.forbidden(cmd.ownerBotId,Action::environment_launch,cmd.environmentId))
                ? Delivery{Delivery::Kind::NotAccepted,{},"Session action denied by installation policy.",true}
                : t3.send(cmd);
            else outcome = t3.reconcile(cmd);
        } catch (const std::exception&) {
            outcome.detail = "T3 communication failed; check the saved action before retrying";
        }
        applyDelivery(id,action,std::move(outcome));
    }

    bool hasSettledRun(const Json& task) const {
        return task["activeRun"].is_null() && hasRun(runRef(task["lastRun"]));
    }

    static constexpr std::size_t maxEvidenceReads = 3;

    struct PeerResult {
        Id taskId;
        std::string kind, environment;
    };

    std::optional<PeerResult> peerResult(const Json& task, const Json& event) const {
        // A peer result is actionable only for the sender and the closing message of that exchange.
        if (event.at("kind") != "bot_result") return std::nullopt;
        auto exchange = db.query(R"SQL(
SELECT p.recipient_task, p.kind, recipient.environment_id
FROM peer_exchanges p JOIN tasks recipient ON recipient.id=p.recipient_task
WHERE p.id=? AND p.sender_task=? AND p.phase='closed' AND p.last_message=?
  AND p.sender_environment=? AND p.sender_revision IS ?
)SQL",{event.at("exchangeId"),task.at("id"),event.at("messageId"),task.at("environmentId"),revision(task)});
        if (!exchange.next()) return std::nullopt;
        return PeerResult{exchange.number(0),exchange.string(1),exchange.string(2)};
    }

    bool canComplete(const Json& task, const Json& event) const {
        if (task.value("replanning",false)) return false;
        if (!task.value("newSessionRequired",false) && hasSettledRun(task) &&
            task.at("lastRun").at("revisionId") == revision(task) &&
            field(settledSessionReport(task),"evidenceStatus") != "report_unavailable") return true;
        const auto peer = peerResult(task,event);
        if (!peer) return false;
        if (task.value("newSessionRequired",false))
            return peer->environment == field(task,"environmentId") && event.at("status") == "completed";
        return task.contains("currentRevision") || (peer->kind == "consult" && event.at("status") == "completed");
    }

    std::vector<std::string> available(const Json& task, const Json& event,
                                       const std::optional<PeerExchange>& peer, bool canSend, bool canRead) {
        if (task.contains("runTargets")) return {"delegate","ask_user"};
        const auto id = task.at("id").get<Id>();
        if (peer && peer->sender == id && peer->phase == "question") return {"reply_to_bot","ask_user"};
        if (!task["activeRun"].is_null() && !field(task["question"],"requestId").empty())
            return {"answer_session_question","ask_user"};
        const bool receiving = peer && peer->recipient == id && peer->phase == "working";
        if (receiving && peer->kind == "consult") return {"reply_to_bot"};
        std::vector<std::string> tools{"delegate",receiving ? "reply_to_bot" : "ask_user"};
        if (hasSettledRun(task)) {
            if (!task.value("newSessionRequired",false)) {
                tools.push_back("follow_up");
                if (task.contains("selectedSession")) std::erase(tools,"delegate");
            }
            if (event.contains("sessionReadViews")) tools.push_back("session_read");
        }
        if (!receiving) {
            std::lock_guard lock(db.mutex);
            if (canComplete(task,event)) tools.push_back("complete");
        }
        if (canRead && event["evidence"].size() < maxEvidenceReads) tools.push_back("read_bot_evidence");
        if (canSend && (!task.contains("selectedSession") || hasSettledRun(task))) tools.push_back("send_bot_message");
        return tools;
    }
    void recordFailure(Json& task, const Json& event, std::string_view detail, bool transient) {
        const auto attempts = event["failures"].get<int>() + 1;
        const auto why = bounded(db.redactor(field(task,"botId")).text(std::string(detail)),2048);
        db.exec("UPDATE events SET failures=?,retry_at=? WHERE id=?",
                {attempts, transient ? options.clock() + (1 << (attempts - 1)) : 0,event["id"]});
        db.journal(task["id"],"model_failure",{{"eventId",event["id"]},{"attempt",attempts},{"text",why}});
        task["feedback"] = why;
        if (attempts >= 3) db.wait(task,"model_failure",{{"text",why}});
    }

    void failure(const Json& task, const Json& event, std::string_view detail, bool transient) {
        change(task,[&](Json& current) {
            recordFailure(current,event,detail,transient);
            return true;
        });
    }

    static constexpr int maxToolErrors = 6;

    void rejectDecision(const Json& task, const Json& event, const TaskDecision& decision, std::string_view message,
                        bool replay = false) {
        change(task,[&](Json& current) {
            const bool unknown = replay && isGitTool(decision.tool) &&
                db.gitEffectStarted(gitInvocation(decision),current.at("id"));
            const auto why = unknown ? "The host outcome is unknown. Check the target before retrying." : bounded(message,2048);
            Json entry = {{"tool",bounded(decision.tool,128)},{"result",{{"ok",false},{"error",why}}},
                          {"eventId",event.at("id")}};
            if (unknown) entry["result"]["delivery"] = "unknown";
            const auto args = decision.args.dump();
            if (args.size() <= 2048) entry["args"] = decision.args;
            else entry["argumentsExcerpt"] = bounded(args,2048);
            db.journal(current["id"],"tool_error",entry);
            db.exec("UPDATE events SET failures=0,retry_at=0 WHERE id=?",{event["id"]});
            current.erase("feedback");
            if (event.at("decisionFeedback").size() + 1 >= maxToolErrors)
                db.wait(current,"model_failure",{{"text",why}});
            return true;
        });
    }

    static std::size_t readOffset(const Json& args) {
        if (args.contains("offset") && (!args["offset"].is_number_integer() || args["offset"].get<Id>() < 0))
            throw Conflict("Invalid history offset");
        return args.value("offset",std::size_t{0});
    }

    Json readExcerpt(std::string_view bot, const ReadRequest& req) {
        const auto scrub = db.redactor(bot);
        const auto excerpt = [&] {
            try {
                return t3.read(req);
            } catch (const ModelError& error) {
                throw HistoryUnavailable(scrub.text(error.what()),error.transient);
            } catch (const std::exception& error) {
                throw HistoryUnavailable(scrub.text(error.what()),false);
            }
        }();
        if ((!excerpt.threadId.empty() && excerpt.threadId != req.threadId) ||
            (!excerpt.runId.empty() && excerpt.runId != req.runId))
            throw Conflict("History source does not match the requested Run");
        scrub.requirePublic({excerpt.itemId,excerpt.nextCursor});
        const auto redacted = scrub.text(excerpt.text);
        auto text = bounded(redacted,8192);
        const bool clipped = text.size() < redacted.size();
        return {{"environmentId",req.environmentId},{"threadId",req.threadId},{"runId",req.runId},
                {"itemId",excerpt.itemId},{"text",std::move(text)},{"more",excerpt.more || clipped},
                {"itemTruncated",excerpt.nextOffset > req.offset || clipped},
                {"nextCursor",excerpt.nextCursor},{"nextOffset",excerpt.nextOffset}};
    }

    bool readEvidence(const Json& task, const Json& event, const Json& args, const Json& usage = Json::object()) {
        ReadRequest req;
        req.environmentId = field(task,"environmentId");
        req.config = task["config"];
        req.threadId = field(args,"threadId");
        req.runId = field(args,"runId");
        req.itemId = field(args,"itemId");
        req.cursor = field(args,"cursor");
        req.view = field(args,"view").empty() ? "messages" : field(args,"view");
        req.offset = readOffset(args);
        if (req.view != "messages" && req.view != "activity") throw Conflict("Unsupported history view");
        const auto& report = event.contains("sessionResult") ? event["sessionResult"] : event;
        const auto active = runRef(report.value("run",Json::object()));
        if (active.threadId == req.threadId && (active.runId == req.runId || field(report,"resultRunId") == req.runId)) {
            req.environmentId = report.at("environmentId");
        } else {
            std::lock_guard lock(db.mutex);
            auto row = db.query("SELECT DISTINCT environment_id FROM runs WHERE task_id=? AND thread_id=? AND run_id=? LIMIT 2",
                                {task["id"],req.threadId,req.runId});
            if (!row.next()) throw Conflict("History reference does not belong to this task");
            req.environmentId = row.string(0);
            if (row.next()) throw Conflict("History reference matches more than one Session");
        }
        if (req.threadId.empty() || req.runId.empty()) throw Conflict("History reference does not belong to this task");
        if (!policy.allows(field(task,"botId"),Resource::sessionRead(req.environmentId,req.threadId)))
            throw Conflict("Session read access required");
        return persistExcerpt(task,event,readExcerpt(field(task,"botId"),req),"session_read",usage);
    }

    bool persistExcerpt(const Json& task, const Json& event, Json entry, std::string_view kind, const Json& usage) {
        return change(task,[&](Json& current) {
            recordEvidenceRead(current,event,std::move(entry),kind,usage);
            return true;
        });
    }

    void recordEvidenceRead(const Json& task, const Json& event, Json entry, std::string_view kind, const Json& usage) {
        auto evidence = event.at("evidence");
        if (evidence.size() >= maxEvidenceReads) throw Conflict("History read allowance exhausted");
        evidence.push_back(entry);
        db.exec("UPDATE events SET evidence=? WHERE id=? AND task_id=?", {evidence,event["id"],task["id"]});
        if (!usage.empty()) entry["usage"] = usage;
        db.journal(task["id"],kind,entry);
    }

    enum class PrefillResult { NotNeeded, Persisted, UnavailableWithSettledReport };

    PrefillResult prefillHistory(const Json& task, const Json& event) {
        if (options.sessionRead != "prefill" || !event["evidence"].empty() ||
            (event["kind"] != "result" && !event.contains("sessionResult")))
            return PrefillResult::NotNeeded;
        const auto& report = event.contains("sessionResult") ? event["sessionResult"] : event;
        const auto run = runRef(report["run"]);
        if (!policy.allows(field(task,"botId"),Resource::sessionRead(report.value("environmentId",field(task,"environmentId")),run.threadId)))
            return PrefillResult::NotNeeded;
        try {
            readEvidence(task,event,{{"threadId",run.threadId},{"runId",run.runId},{"view","activity"}});
            return PrefillResult::Persisted;
        } catch (const HistoryUnavailable&) {
            return PrefillResult::UnavailableWithSettledReport;
        }
    }

    struct WorkPrompt {
        std::string instruction;
        Json pending = Json::array(), contextIds = Json::object(), resultReferences = Json::array();
    };

    struct SessionContract {
        std::string intent, doneWhen;
        Id answersAfter;
    };

    SessionContract sessionContract(const Json& task) {
        auto row = db.query(R"SQL(
SELECT r.intent,r.done_when,
       coalesce((SELECT max(j.id) FROM journal j WHERE j.task_id=r.task_id AND
         ((r.number=1 AND j.kind='task_contract_changed') OR
          (r.number>1 AND j.kind='task_revision' AND json_extract(j.data,'$.revisionId')=r.id))),
         r.source_message_id,0)
FROM task_revisions r WHERE r.id=? AND r.task_id=?
)SQL",{revision(task),task.at("id")});
        if (!row.next()) throw Conflict("The new Session needs an established task revision");
        return {row.string(0),row.string(1),row.number(2)};
    }

    Json appendSessionDirections(std::string& instruction, const Json& task, const Json& pending, Id answersAfter) {
        using InstructionId = std::pair<Id,Id>;
        std::set<InstructionId> pendingIds;
        for (const auto& item : pending)
            pendingIds.emplace(item.at("messageId"),item.at("step"));
        auto rows = db.query(R"SQL(
SELECT j.id,j.kind,j.data,json_extract(w.data,'$.text'),
       CASE j.kind WHEN 'instruction_accepted' THEN EXISTS (
         SELECT 1 FROM journal d WHERE d.task_id=j.task_id AND d.kind='instruction_delivered'
           AND json_extract(d.data,'$.messageId')=json_extract(j.data,'$.messageId')
           AND json_extract(d.data,'$.step')=json_extract(j.data,'$.step')
           AND json_extract(d.data,'$.revisionId')=?2)
       ELSE 0 END
FROM journal j
LEFT JOIN waits w ON j.kind='user_answer' AND w.id=json_extract(j.data,'$.waitId') AND w.task_id=j.task_id
WHERE j.task_id=?1 AND
      (j.kind='instruction_accepted' OR
       (j.kind='user_answer' AND j.id>?3 AND w.resolution='answer'))
ORDER BY j.id
)SQL",{task.at("id"),revision(task),answersAfter});
        Json sources = Json::array();
        bool first = true;
        while (rows.next()) {
            const auto kind = rows.string(1);
            const auto item = rows.json(2);
            const bool answer = kind == "user_answer";
            if (kind == "instruction_accepted" && !rows.number(4) &&
                !pendingIds.contains({item.at("messageId"),item.at("step")})) continue;
            if (std::exchange(first,false))
                instruction += "\n\nUser decisions for this task, oldest first. Apply later directions where they replace "
                               "earlier ones, under the current task intent and completion criteria above.";
            instruction += answer ? "\n\nUser answer" : "\n\nAccepted instruction";
            instruction += " (record " + std::to_string(rows.number(0));
            if (item.contains("messageId")) instruction += ", message " + item.at("messageId").dump();
            if (item.contains("step")) instruction += ", step " + item.at("step").dump();
            if (answer) instruction += ", wait " + item.at("waitId").dump();
            instruction += "):\n";
            if (answer) instruction += rows.string(3) + "\nAnswer: ";
            instruction += field(item,"text");
            if (instruction.size() > maxSessionPromptBytes) throw SessionContextLimit{};
            if (item.contains("source")) sources.push_back(item.at("source"));
        }
        return sources;
    }

    WorkPrompt workPrompt(const Json& task, const Json& event, std::size_t actionIdBytes,
                          const SessionContract* contract = nullptr) {
        WorkPrompt prompt;
        auto& [instruction,pending,contextIds,resultReferences] = prompt;
        const auto scrub = db.redactor(field(task,"botId"));
        // Use the same allowance before and after the model supplies its instruction and criteria.
        std::size_t overhead = 8000 + 512 + std::max<std::size_t>(64,actionIdBytes);
        if (!contract && event.contains("userAnswer")) {
            contextIds["answerEventId"] = event.at("id");
            instruction += "\n\nUser answer:\n" + field(event,"userAnswer");
        }
        const bool moved = task.value("newSessionRequired",false);
        if (contract || moved || task["lastRun"].is_null() || task["lastRun"].at("revisionId") != revision(task)) {
            const auto criteria = contract ? contract->doneWhen : effectiveDoneWhen(task);
            instruction += "\n\nTask intent:\n" + (contract ? contract->intent : taskIntent(task)) +
                "\n\nCompletion criteria:\n" + criteria;
            overhead += std::max<std::size_t>(criteria.size(),8000) - criteria.size();
        }
        if (contract || moved) {
            if (field(event,"kind") == "bot_answer") {
                contextIds["peerMessageId"] = event.at("messageId");
                instruction += "\n\nAnswer from the requesting Bot:\n" + field(event,"text");
            }
        }
        if (task.contains("sourceTaskId") && task["lastRun"].is_null()) {
            contextIds["sourceMessageId"] = task.at("sourceMessageId");
            const auto request = field(task,"request");
            instruction += "\n\nUser request:\n" + request;
        }
        if (const auto restart = task.find("restartContext"); restart != task.end() && task["lastRun"].is_null()) {
            const auto& delivered = restart->at("deliveredInstructions");
            if (!delivered.empty()) instruction += "\n\nEarlier user instructions delivered before cancellation (reference only):";
            for (const auto& item : delivered) instruction += "\n\n" + field(item,"text");
        }
        SourceQuotes quotes{scrub};
        const auto quoteSource = [&](const Json& source, std::size_t limit = 8192) {
            const auto safe = scrub.record(source,SecretRedactor::Fields::external);
            const auto quote = quotes.quote(safe,limit);
            if (quote.empty()) return;
            instruction += quote;
            auto reference = safe;
            reference.erase("text");
            resultReferences.push_back(std::move(reference));
        };
        constexpr std::size_t recentReportLimit = 3, reportExcerptBytes = 2000;
        const auto recentReportBudget = moved ? quotes.remaining / 2 : 0;
        quotes.remaining -= recentReportBudget;
        if (task.contains("sourceTaskId") && (moved || task["lastRun"].is_null())) {
            for (const auto& source : task.at("sourceResultRefs"))
                if (source.at("taskId") == task.at("sourceTaskId")) quoteSource(source);
        }
        if (task.contains("workflowId")) {
            for (const auto& source : task.at("sourceResultRefs")) quoteSource(source);
            db.appendStageRequirements(instruction,task);
        }
        quotes.remaining += recentReportBudget;
        if (moved) {
            auto reports = db.query("SELECT id,data FROM journal WHERE task_id=? AND kind='report' ORDER BY id DESC LIMIT ?",
                                    {task.at("id"),recentReportLimit});
            auto remainingReports = recentReportLimit;
            while (reports.next()) {
                const auto report = reports.json(1);
                quoteSource({{"taskId",task.at("id")},{"messageId",reports.number(0)},{"run",report.at("run")},
                             {"resultRunId",report.at("resultRunId")},{"text",field(report,"report")}},
                            std::min(reportExcerptBytes,quotes.remaining/remainingReports));
                --remainingReports;
            }
        }
        overhead += instruction.size();
        const bool resumedTask = task.contains("restartContext") && task["lastRun"].is_null();
        if (contract && !resumedTask)
            pending = task.value("pendingInstructions",Json::array());
        else
            pending = pendingBatch(task,overhead < maxSessionPromptBytes ? maxSessionPromptBytes - overhead : 0,quotes);
        if (contract) {
            const auto sources = appendSessionDirections(instruction,task,pending,contract->answersAfter);
            if (resumedTask && hasPending(task) && pending.size() < task.at("pendingInstructions").size())
                instruction += "\n\nFlock will send the remaining inherited instructions in follow-up Runs in this Session. "
                               "Work only on the instructions included in this Run until the rest arrive.";
            for (const auto& source : sources) quoteSource(source);
        } else {
            includeInstructions(instruction,pending);
            for (const auto& item : pending)
                if (item.contains("source")) quoteSource(item.at("source"));
        }
        return prompt;
    }

    void prepareDelegation(const Json& task, Json& action) {
        auto instruction = action.at("instructionBase").get<std::string>();
        const auto contract = sessionContract(task);
        const auto event = db.currentEvent(task);
        auto prompt = workPrompt(task,event,field(action,"id").size(),&contract);
        const auto scrub = db.redactor(field(task,"botId"));
        instruction += prompt.instruction;
        action["instruction"] = scrub.text(instruction);
        action["instructions"] = scrub.record(std::move(prompt.pending),SecretRedactor::Fields::external);
        action["contextIds"] = std::move(prompt.contextIds);
        action["sourceResultRefs"] = std::move(prompt.resultReferences);
    }

    void queueAction(Json& task, const std::string& action, std::string_view tool, std::string instruction,
                     const Json& args, const Json& event, bool charge, const Json* check = nullptr) {
        auto target = tool == "follow_up" ? task["lastRun"] : task["activeRun"];
        if (tool == "delegate" && task.contains("selectedSession")) {
            if (task.value("newSessionRequired",false)) task.erase("selectedSession");
            else {
                const auto& selected = task.at("selectedSession");
                target = runJson({field(selected,"threadId"),{},{}});
                tool = "start_in_session";
            }
        }
        WorkPrompt prompt;
        if (tool == "follow_up") {
            prompt = workPrompt(task,event,action.size());
            const auto scrub = db.redactor(field(task,"botId"));
            prompt.pending = scrub.record(std::move(prompt.pending),SecretRedactor::Fields::external);
            instruction += prompt.instruction;
            instruction = sessionPrompt(action,scrub.text(instruction));
        }
        if (tool == "delegate" || tool == "follow_up" || tool == "start_in_session") task["deadline"] = 0;
        const auto& route = check ? *check : task;
        Json out = {{"id",action},{"tool",tool},{"environmentId",route.at("environmentId")},{"projectId",route.at("projectId")},
            {"target",std::move(target)},
            {"instruction",std::move(instruction)},{"questionId",field(args,"requestId")},
            {"answers",args.value("answers",Json::object())},{"revisionId",revision(task)},
            {"instructions",prompt.pending},{"contextIds",prompt.contextIds},{"sourceResultRefs",prompt.resultReferences},{"chargesBudget",charge}};
        if (tool == "delegate" || tool == "start_in_session") {
            if (check) {
                out["expectedCommit"] = task.at("requestedCommit");
                out["instruction"] = field(out,"instruction") +
                    "\n\nVerify the requested commit " + field(task,"requestedCommit") +
                    ". Use a command tool to run exactly git rev-parse --verify HEAD as a standalone command "
                    "before doing the work and again after running the checks. Stop and report a mismatch if HEAD differs. "
                    "Keep the checkout at the requested commit and report the checks and their outcomes.";
            }
            out["instructionBase"] = db.redactor(field(task,"botId")).text(field(out,"instruction"));
            out.erase("instruction");
        }
        db.exec("INSERT INTO outbox(id,task_id,phase,data) VALUES(?,?,'ready',?)", {action,task["id"],out});
        if (check) return;
        task["actionId"] = action;
        task["state"] = "sending";
    }

    void queueChecks(Json& task, const std::string& first, std::string_view instruction, const Json& args,
                     const Json& event, bool charge) {
        auto results = task.at("runTargets");
        for (auto it = results.begin(); it != results.end(); ++it) {
            auto& run = *it;
            const auto key = it == results.begin() ? first : actionId();
            run["actionId"] = key;
            run["status"] = "queued";
            queueAction(task,key,"delegate",std::string(instruction),args,event,charge,&run);
        }
        task["runResults"] = std::move(results);
        task["state"] = "running";
    }

    bool admitWork(Json& task, const TaskDecision& decision, bool& charge) {
        charge = !task["lastRun"].is_null() || db.query(
            "SELECT 1 FROM peer_exchanges WHERE sender_task=? AND kind='work' LIMIT 1",{task["id"]}).next();
        if (!charge) return true;
        if (task["remaining"] == 0) {
            db.wait(task,"budget",{{"text","The additional work allowance is exhausted"},{"attempted",decision.tool},
                                  {"decisionReason",decision.reason},{"usage",decision.usage}});
            return false;
        }
        task["remaining"] = task["remaining"].get<int>() - 1;
        task["additionalUsed"] = task["additionalUsed"].get<int>() + 1;
        return true;
    }

    void establishCriteria(Json& task, const Json& args) {
        if (task.contains("currentRevision")) return;
        const auto source = task.value("criteriaSourceMessageId",task.value("sourceMessageId",Id{0}));
        db.addRevision(task,taskIntent(task),task.value("requestedDoneWhen",field(args,"doneWhen")),source);
        task.erase("requestedDoneWhen");
        task.erase("criteriaSourceMessageId");
    }

    std::vector<PeerBot> peersFor(std::string_view bot) const {
        if (!options.peers) return {};
        return options.peers(bot);
    }

    PeerBot allowedPeer(std::string_view sender, std::string_view botId) const {
        if (botId.empty() || sender == botId) throw Conflict("Choose an allowed peer Bot");
        const auto peers = peersFor(sender);
        const auto peer = std::find_if(peers.begin(),peers.end(),[&](const PeerBot& p) { return p.intake.botId == botId; });
        if (peer == peers.end()) throw Conflict("The destination Bot is unavailable or not allowed");
        return *peer;
    }

    Json peerMessage(const PeerExchange& peer, const Json& from, const Json& to, std::string_view type,
                     std::string_view body, std::string_view messageId) {
        Json message = {{"exchangeId",peer.id},{"messageId",messageId},{"fromBotId",from["botId"]},
            {"toBotId",to["botId"]},{"messageType",type},{"requestKind",peer.kind},{"text",body}};
        if (!peer.lastMessage.empty()) message["replyTo"] = peer.lastMessage;
        return message;
    }

    // The caller owns the transaction and saves the acting task. This writes the other participant.
    void transitionPeer(const PeerExchange& peer, Json& actor, Json& task, Json message) {
        const auto type = field(message,"messageType");
        const bool closing = message.contains("status");
        const auto kind = closing ? "bot_result" : "bot_" + type;
        const auto phase = closing ? "closed" : type == "question" ? "question" : "working";
        if (type == "request")
            db.exec("INSERT INTO peer_exchanges(id,kind,phase,last_message,sender_task,recipient_task,"
                    "sender_environment,sender_revision) VALUES(?,?,?,?,?,?,?,?)",
                    {peer.id,peer.kind,phase,message["messageId"],peer.sender,peer.recipient,actor["environmentId"],revision(actor)});
        else
            db.exec("UPDATE peer_exchanges SET phase=?,last_message=? WHERE id=?",{phase,message["messageId"],peer.id});
        db.peerMessage(peer,message);
        consumeEvent(actor);
        if (!closing) actor["state"] = "peer_waiting";
        if (terminal(task) || task["state"] == "cancelling") return;
        if (closing) db.clearWait(task,kind);
        consumeEvent(task);
        if (message.contains("evidence")) {
            message["references"] = std::move(message["evidence"]);
            message.erase("evidence");
        }
        message["kind"] = kind;
        const auto key = "bot/" + field(message,"messageId");
        db.event(task,key,std::move(message));
        if (task["state"] != "queued") task["state"] = "deciding";
        saveTask(task);
    }

    void recordDecision(const Json& task, const Json& event, const TaskDecision& decision, std::string_view action) {
        db.journal(task["id"],"decision",{{"actionId",action},{"tool",decision.tool},{"args",decision.args},
            {"reason",decision.reason},{"eventId",event["id"]},{"evidence",event["evidence"]},{"usage",decision.usage},
            {"revisionId",revision(task)}});
    }

    Json forwardableUserMessage(const Json& task) const {
        const auto sourceId = task.value("sourceMessageId",Id{0});
        if (!sourceId || !task.value("sourceMessageCoversFullRequest",false)) return nullptr;
        // A compound or clarified request may not represent one complete user message.
        auto row = db.query(R"SQL(
SELECT m.text,j.time FROM messages m JOIN journal j ON j.id=m.id
WHERE m.id=? AND m.bot_id=? AND j.kind='conversation_message'
)SQL",{sourceId,task.at("botId")});
        if (!row.next()) return nullptr;
        return {{"text",row.string(0)},{"time",row.number(1)}};
    }

    void sendPeer(Json& sender, const Json& event, const TaskDecision& decision, PeerBot peer, const std::string& action) {
        if (db.peerExchange(sender["id"])) throw Conflict("Finish the current Bot exchange first");
        const auto& args = decision.args;
        const auto note = field(args,"text");
        Json forwarded;
        if (args.value("forwardUserMessage",false)) {
            forwarded = forwardableUserMessage(sender);
            if (forwarded.is_null()) throw Conflict("This task has no complete user message to forward");
        }
        const auto kind = field(decision.args,"kind");
        const bool work = kind == "work";
        bool charge = false;
        if (work) {
            if (!sender["activeRun"].is_null() || !field(sender,"actionId").empty())
                throw Conflict("Let the assigned Run settle before requesting another Bot's work");
            peer.intake.environmentId = db.defaultT3Environment(peer.intake.botId,peer.intake.environmentId,
                                                               field(sender,"environmentId"));
            if (peer.intake.environmentId.empty())
                throw Conflict("Choose a T3 environment for the destination Bot in its configuration");
            if (!peer.intake.projectId.empty() &&
                db.peerWorkRouteWouldCycle(sender.at("id"),peer.intake.environmentId,peer.intake.projectId))
                throw Conflict("This work request would make two project queues wait for one another");
            if (!admitWork(sender,decision,charge)) return;
            establishCriteria(sender,args);
        }
        peer.intake.request = note.empty() ? field(forwarded,"text") : note;
        const auto source = work ? TaskSource::handoff : TaskSource::consultation;
        const auto id = db.submit(peer.intake,0,nullptr,source);
        auto recipient = db.load(id);
        const PeerExchange exchange{action,kind,"working","",sender["id"],id};
        auto message = peerMessage(exchange,sender,recipient,"request",note,action);
        message["chargesBudget"] = charge;
        if (!forwarded.is_null()) message["forwardedMessage"] = std::move(forwarded);
        transitionPeer(exchange,sender,recipient,std::move(message));
        recordDecision(sender,event,decision,action);
    }

    Json settledSessionReport(const Json& task, Id* id = nullptr) const {
        const auto run = runRef(task["lastRun"]);
        if (!hasRun(run)) return nullptr;
        auto report = db.query("SELECT id,data FROM journal WHERE task_id=? AND kind='report' ORDER BY id DESC LIMIT 1",
                               {task["id"]});
        if (!report.next()) return nullptr;
        auto data = report.json(1);
        const auto reported = runRef(data["run"]);
        if (reported.threadId != run.threadId || reported.runId != run.runId) return nullptr;
        if (id) *id = report.number(0);
        return data;
    }

    struct ReportRetry {
        Id journalId = 0;
        Json report;
    };

    std::optional<ReportRetry> reportRetry(const Json& task, const Json& event) const {
        if (!hasSettledRun(task) || task.value("replanning",false) || task.value("newSessionRequired",false) ||
            task.at("lastRun").at("revisionId") != revision(task)) return std::nullopt;
        ReportRetry retry;
        retry.report = settledSessionReport(task,&retry.journalId);
        const auto& report = retry.report;
        if (field(report,"evidenceStatus") != "report_unavailable" || report.at("run") != task.at("lastRun") ||
            report.at("environmentId") != task.at("environmentId") || field(report,"resultRunId").empty())
            return std::nullopt;
        if (event.at("kind") == "result") {
            for (const auto* key : {"environmentId","run","resultRunId","status","revisionId"})
                if (!event.contains(key) || event.at(key) != report.at(key)) return std::nullopt;
        }
        // Only a Run settled with a proven resultRunId enters the original report retry path.
        if (!db.query(R"SQL(
SELECT 1 FROM journal WHERE task_id=? AND kind='run_settled'
AND json_extract(data,'$.environmentId')=? AND json_extract(data,'$.run')=json(?)
AND json_extract(data,'$.resultRunId')=? AND json_extract(data,'$.status')=?
)SQL",{task.at("id"),report.at("environmentId"),report.at("run"),report.at("resultRunId"),report.at("status")}).next())
            return std::nullopt;
        return retry;
    }

    void retryRunReport(const Json& task, const Json& event, const TaskDecision& decision) {
        const auto retry = [&] {
            std::lock_guard lock(db.mutex);
            return reportRetry(task,event);
        }();
        const auto& args = decision.args;
        if (!retry || args.contains("itemId") || args.contains("cursor") || args.contains("offset") ||
            field(args,"threadId") != field(retry->report.at("run"),"threadId") ||
            field(args,"runId") != field(retry->report,"resultRunId"))
            throw Conflict("Choose the exact settled result Run to retry its terminal report");
        const auto env = field(retry->report,"environmentId");
        const auto thread = field(retry->report.at("run"),"threadId");
        if (!policy.allows(field(task,"botId"),Resource::sessionRead(env,thread)))
            throw Conflict("Session read access required");
        const auto evidence = fetchRunEvidence(task,retry->report);
        const bool available = evidence.availability != RunEvidence::Availability::ReportUnavailable;
        change(task,[&](Json& current) {
            const auto latest = db.currentEvent(current);
            if (latest.is_null() || latest.at("consumed") == true || current.at("state") != "deciding") return false;
            const auto target = reportRetry(current,latest);
            if (!target || target->journalId != retry->journalId || target->report != retry->report) return false;
            if (available) {
                auto report = target->report;
                setReportEvidence(report,evidence);
                db.exec("UPDATE journal SET data=? WHERE id=?",{report,target->journalId});
                if (latest.at("kind") == "result") {
                    auto row = db.query("SELECT data FROM events WHERE id=? AND task_id=?",{latest.at("id"),current.at("id")});
                    if (!row.next()) throw Conflict("The settled result event is no longer available");
                    auto data = row.json(0);
                    setReportEvidence(data,evidence);
                    db.exec("UPDATE events SET data=? WHERE id=? AND task_id=?",{data,latest.at("id"),current.at("id")});
                }
            }
            recordEvidenceRead(current,latest,{{"environmentId",env},{"threadId",thread},
                {"runId",target->report.at("resultRunId")},{"view","report"},
                {"reportAvailable",available},{"text",bounded(evidence.detail,2048)}},"session_read",decision.usage);
            return true;
        });
    }

    Json sessionResultReference(const Json& report) const {
        if (report.is_null()) return nullptr;
        const auto run = runRef(report.value("run",Json(nullptr)));
        if (!hasRun(run)) return nullptr;
        return {{"environmentId",report.at("environmentId")},{"threadId",run.threadId},{"runId",field(report,"resultRunId")},
                {"itemId",field(report,"itemId")},{"link",run.link},{"status",field(report,"status")}};
    }

    void finishPeer(const PeerExchange& exchange, Json& recipient, Json sender, std::string_view status,
                    std::string_view text, const std::string& action) {
        const auto type = status == "completed" ? std::string_view{"result"} : status;
        auto message = peerMessage(exchange,recipient,sender,type,text,action);
        message["status"] = status;
        if (status != "cancelled") {
            const auto ref = sessionResultReference(settledSessionReport(recipient));
            if (!ref.is_null()) message["evidence"] = Json::array({ref});
        }
        transitionPeer(exchange,recipient,sender,std::move(message));
    }

    Json replyKinds(const Json& task, const PeerExchange& peer) const {
        if (peer.sender == task["id"] && peer.phase == "question") return Json::array({"answer"});
        if (peer.recipient != task["id"] || peer.phase != "working") return Json::array();
        Json kinds = {"question"};
        const bool revisedWork = peer.kind == "work" && (hasPending(task) || task.value("replanning",false) ||
            task.value("newSessionRequired",false) || (!task["lastRun"].is_null() &&
                task["lastRun"].at("revisionId") != revision(task)));
        if (!revisedWork) {
            kinds.push_back("failed");
            if (field(settledSessionReport(task),"evidenceStatus") != "report_unavailable") kinds.push_back("result");
        }
        return kinds;
    }

    bool replyPeer(Json& task, const Json& event, const TaskDecision& decision, const std::string& action) {
        const auto peer = db.peerExchange(task["id"]);
        if (!peer) throw Conflict("The Bot exchange has closed");
        const auto type = field(decision.args,"kind");
        const auto body = field(decision.args,"text");
        const auto kinds = replyKinds(task,*peer);
        if (std::find(kinds.begin(),kinds.end(),type) == kinds.end())
            throw Conflict("This reply does not match the open exchange and current task revision");
        const bool answering = type == "answer";
        const auto otherId = answering ? peer->recipient : peer->sender;
        auto other = db.load(otherId);
        if (terminal(other) || other["state"] != "peer_waiting") throw Conflict("The other Bot is no longer waiting");
        const bool closing = type == "result" || type == "failed";
        if (closing && db.conversationPending(field(task,"botId"))) return false;
        recordDecision(task,event,decision,action);
        if (closing) {
            const auto status = type == "result" ? "completed" : "failed";
            task["state"] = "completed";
            task["peerOutcome"] = status;
            finishPeer(*peer,task,std::move(other),status,body,action);
            db.journal(task["id"],"peer_completed",{{"text",body},{"status",status},
                {"reason",decision.reason},{"doneWhen",task["doneWhen"]}});
        } else {
            transitionPeer(*peer,task,other,peerMessage(*peer,task,other,type,body,action));
        }
        return true;
    }

    void readPeerEvidence(const Json& task, const Json& event, const TaskDecision& decision) {
        const auto& args = decision.args;
        const auto messageId = field(args,"messageId");
        if (event["kind"] != "bot_result" || messageId.empty() || event["messageId"] != messageId)
            throw Conflict("Choose the Bot result for the current exchange");
        allowedPeer(field(task,"botId"),field(event,"fromBotId"));
        const auto refs = event.value("references",Json::array());
        if (refs.empty()) throw Conflict("The Bot result has no shared Session history");
        const auto& ref = refs.front();
        ReadRequest req;
        req.environmentId = field(ref,"environmentId");
        req.threadId = field(ref,"threadId");
        req.runId = field(ref,"runId");
        req.itemId = field(args,"itemId");
        req.cursor = field(args,"cursor");
        req.offset = readOffset(args);
        {
            std::lock_guard lock(db.mutex);
            auto row = db.query("SELECT 1 FROM peer_exchanges WHERE id=? AND sender_task=? AND last_message=?",
                                {event["exchangeId"],task["id"],messageId});
            if (!row.next()) throw Conflict("The Bot result does not belong to this task");
            req.config = task.at("config");
        }
        if (req.threadId.empty() || req.runId.empty()) throw Conflict("The Bot result has no readable Run");
        if (!policy.allows(field(task,"botId"),Resource::sessionRead(req.environmentId,req.threadId))) throw Conflict("Session read access required");
        auto entry = readExcerpt(field(task,"botId"),req);
        entry["messageId"] = messageId;
        entry["exchangeId"] = event["exchangeId"];
        persistExcerpt(task,event,std::move(entry),"read_bot_evidence",decision.usage);
    }

    void acceptReplyOrCompletion(const Json& snapshot, const Json& event, const TaskDecision& decision) {
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        const auto id = snapshot["id"].get<Id>();
        auto current = db.load(id);
        if (current["generation"] != snapshot["generation"] || current["eventId"] != event["id"] ||
            revision(current) != revision(snapshot) || current["state"] != "deciding") return;
        if (decision.tool == "reply_to_bot") {
            if (replyPeer(current,event,decision,actionId())) saveTask(current);
        } else {
            if (db.peerExchange(id) || !canComplete(current,event)) return;
            complete(current,event,decision);
        }
        transaction.commit();
    }

    void addCompletionProvenance(Json& completion, const Json& task, const Json& event) {
        const auto peer = peerResult(task,event);
        if (!peer) {
            const auto& report = event.contains("sessionResult") ? event.at("sessionResult") : event;
            if (auto source = sessionResultReference(report); !source.is_null())
                completion["source"] = std::move(source);
            return;
        }

        completion["dependency"] = {{"exchangeId",event.at("exchangeId")},{"messageId",event.at("messageId")},
            {"taskId",peer->taskId},{"botId",event.at("fromBotId")},{"requestKind",peer->kind},{"status",event.at("status")}};
        const auto allowed = peersFor(field(task,"botId"));
        const auto peerBot = field(event,"fromBotId");
        const bool peerAllowed = std::ranges::any_of(allowed,[&](const PeerBot& candidate) {
            return candidate.intake.botId == peerBot;
        });
        auto refs = Json::array();
        const auto addRef = [&](const Json& record, std::string_view kind, bool fromPeer) {
            const auto environment = field(record,"environmentId"), thread = field(record,"threadId"), run = field(record,"runId");
            if ((fromPeer && !peerAllowed) || run.empty() ||
                !policy.allows(field(task,"botId"),Resource::sessionRead(environment,thread))) return;
            Json ref = {{"environmentId",environment},{"threadId",thread},{"runId",run},{"kind",kind}};
            if (const auto item = field(record,"itemId"); !item.empty()) ref["itemId"] = item;
            if (fromPeer) ref["messageId"] = event.at("messageId");
            if (std::ranges::find(refs,ref) == refs.end()) refs.push_back(std::move(ref));
        };
        for (const auto& record : event.value("references",Json::array())) addRef(record,"peer_report",true);
        auto reads = db.query(R"SQL(
SELECT data FROM journal WHERE task_id=? AND kind='read_bot_evidence'
  AND json_extract(data,'$.exchangeId')=? AND json_extract(data,'$.messageId')=? ORDER BY id
)SQL",{task.at("id"),event.at("exchangeId"),event.at("messageId")});
        while (reads.next()) addRef(reads.json(0),"session_read",true);
        for (const auto& record : event.at("evidence"))
            if (!record.contains("exchangeId")) addRef(record,"session_read",false);
        completion["evidenceRefs"] = std::move(refs);
    }

    void complete(Json& current, const Json& event, const TaskDecision& decision) {
        const auto pending = current.value("pendingInstructions",Json::array());
        auto ids = Json::array();
        for (const auto& item : pending) ids.push_back({{"messageId",item.at("messageId")},{"step",item.at("step")}});
        const bool requiresAcknowledgment = !pending.empty();
        if (requiresAcknowledgment != decision.args.contains("acknowledgedInstructions") ||
            decision.args.value("acknowledgedInstructions",Json::array()) != ids)
            throw Conflict("Completion must acknowledge each pending instruction by messageId and step, oldest first");
        if (db.conversationPending(field(current,"botId"))) return;
        consumeEvent(current);
        recordDecision(current,event,decision,actionId());
        current.erase("pendingInstructions");
        current.erase("newSessionRequired");
        current["state"] = "completed";
        Json completion = {{"text",field(decision.args,"report")},{"reason",decision.reason},
                           {"doneWhen",current["doneWhen"]},{"revisionId",revision(current)}};
        addCompletionProvenance(completion,current,event);
        db.journal(current["id"],"completed",completion);
        saveTask(current);
    }

    void accept(const Json& snapshot, const Json& event, const TaskDecision& decision,
                std::string_view approvedInvocation = {}) {
        const auto& args = decision.args;
        const auto& tool = decision.tool;
        if (isGitTool(tool)) {
            Json result;
            const auto invocation = approvedInvocation.empty() ? gitInvocation(decision) : std::string(approvedInvocation);
            try {
                result = db.gitTool(*options.gitHost,field(snapshot,"botId"),snapshot.at("id"),snapshot,decision,
                                    invocation,snapshot.at("generation").get<Id>());
            } catch (const StorageError&) { throw; }
            catch (const std::exception& error) { result = {{"ok",false},{"error",db.sharedRedactor().text(error.what())}}; }
            if (result.value("policyWaiting",false)) return;
            change(snapshot,[&](Json& current) {
                db.exec("UPDATE policy_approvals SET resume=NULL WHERE operation=?",
                        {"git/" + current.at("id").dump() + "/" + invocation});
                auto row = db.query("SELECT data FROM events WHERE id=? AND task_id=?",{event.at("id"),current.at("id")});
                if (!row.next()) return false;
                auto data = row.json(0);
                if (!data.contains("gitResults")) data["gitResults"] = Json::array();
                if (result.contains("readId"))
                    data["gitResults"].push_back({{"tool",tool},{"readId",result.at("readId")}});
                else data["gitResults"].push_back(db.redactor(field(current,"botId")).record(
                    Json{{"tool",tool},{"result",std::move(result)}}));
                if (data["gitResults"].size() > 4) data["gitResults"].erase(data["gitResults"].begin());
                db.exec("UPDATE events SET data=? WHERE id=?",{data,event.at("id")});
                return true;
            });
            return;
        }
        static const std::map<std::string_view,std::set<std::string_view>> arguments{
            {"delegate",{"instruction","doneWhen"}},
            {"answer_session_question",{"requestId","answers"}},
            {"follow_up",{"instruction"}},
            {"complete",{"report","acknowledgedInstructions"}},
            {"ask_user",{"question","choices"}},
            {"session_read",{"threadId","runId","itemId","cursor","view","offset"}},
            {"send_bot_message",{"botId","kind","text","doneWhen","forwardUserMessage"}},
            {"reply_to_bot",{"kind","text"}},
            {"read_bot_evidence",{"messageId","itemId","cursor","offset"}}
        };
        for (auto item = args.begin(); item != args.end(); ++item) {
            if (!arguments.at(tool).contains(item.key())) throw Conflict("Unknown action argument");
        }
        const bool setsCriteria = tool == "delegate" || (tool == "send_bot_message" && field(args,"kind") == "work");
        const bool requireCriteria = setsCriteria && needsDoneWhen(snapshot);
        if (requireCriteria != args.contains("doneWhen") || (requireCriteria && !present(field(args,"doneWhen"))))
            throw Conflict("Only first work without saved criteria requires doneWhen");
        if (tool == "session_read") {
            if (field(args,"view") == "report") return retryRunReport(snapshot,event,decision);
            if (options.sessionRead != "on_demand") throw Conflict("Only the terminal report can be read for this event");
            readEvidence(snapshot,event,args,decision.usage);
            return;
        }
        if (tool == "read_bot_evidence") return readPeerEvidence(snapshot,event,decision);
        if (tool == "send_bot_message" || tool == "reply_to_bot") {
            if (args.contains("forwardUserMessage") && !args.at("forwardUserMessage").is_boolean())
                throw Conflict("forwardUserMessage must be a boolean");
            if (args.contains("forwardUserMessage") && !snapshot.value("sourceMessageCoversFullRequest",false))
                throw Conflict("Forwarding is unavailable for this request");
            const auto text = field(args,"text");
            if ((args.contains("text") && !args.at("text").is_string()) ||
                (!present(text) && !(text.empty() && args.value("forwardUserMessage",false))))
                throw Conflict("A Bot message requires text or a forwarded user message");
            if (tool == "reply_to_bot") return acceptReplyOrCompletion(snapshot,event,decision);
            const auto kind = field(args,"kind");
            if (kind != "consult" && kind != "work") throw Conflict("Choose a consultation or work request");
            auto peer = allowedPeer(field(snapshot,"botId"),field(args,"botId"));
            change(snapshot,[&](Json& current) { sendPeer(current,event,decision,std::move(peer),actionId()); return true; });
            return;
        }
        const bool work = tool == "delegate" || tool == "follow_up";
        const auto instruction = field(args,"instruction");
        if (work && !present(instruction)) throw Conflict("A delegate/follow-up instruction is required");
        if (tool == "ask_user") {
            if (!present(field(args,"question"))) throw Conflict("A user question is required");
            if (args.contains("choices")) {
                const auto& choices = args["choices"];
                if (!choices.is_array() || choices.size() > 12 ||
                    std::any_of(choices.begin(),choices.end(),[](const Json& value) {
                        return !value.is_string() || !present(value.get<std::string>(),200);
                    })) throw Conflict("Expected up to twelve short textual choices");
            }
        }
        if (tool == "complete") {
            if (!present(field(args,"report"))) throw Conflict("A completion report is required");
            return acceptReplyOrCompletion(snapshot,event,decision);
        }
        if (tool == "answer_session_question") {
            if (field(args,"requestId") != field(snapshot["question"],"requestId"))
                throw Conflict("The Session question no longer matches");
            sessionQuestionAnswers(snapshot["question"]["fields"],args.value("answers",Json::object()));
        }
        change(snapshot,[&](Json& current) {
            if (tool == "follow_up" && current.value("newSessionRequired",false))
                throw Conflict("Start a new Session in the current environment");
            bool charge = false;
            if (work && !admitWork(current,decision,charge)) return true;
            if (work) establishCriteria(current,args);
            consumeEvent(current);
            const auto action = actionId();
            recordDecision(current,event,decision,action);
            if (tool == "ask_user") {
                db.wait(current,"user_question",{{"text",field(args,"question")},{"decisionReason",decision.reason},
                    {"questionId",field(current["question"],"requestId")},{"choices",args.value("choices",Json::array())}});
                if (current["question"].is_object()) current["question"]["target"] = questionTarget(current);
            } else if (work && current.contains("runTargets")) {
                queueChecks(current,action,instruction,args,event,charge);
            } else {
                queueAction(current,action,tool,instruction,args,event,charge);
            }
            return true;
        });
    }

    bool forwardedQuestionAnswer(const Json& task, const Json& target) {
        std::lock_guard lock(db.mutex);
        const auto event = db.currentEvent(task);
        const auto peer = db.peerExchange(task["id"]);
        if (!peer || peer->recipient != task["id"] || peer->kind != "work" || peer->phase != "working" ||
            event.is_null() || event["consumed"] == true || field(event,"kind") != "bot_answer" ||
            field(event,"messageType") != "answer" || field(event,"requestKind") != peer->kind ||
            field(event,"exchangeId") != peer->id || field(event,"messageId") != peer->lastMessage ||
            field(event,"toBotId") != field(task,"botId")) return false;
        const auto sender = db.load(peer->sender);
        if (field(event,"fromBotId") != field(sender,"botId")) return false;
        auto original = db.query("SELECT data FROM events WHERE id=? AND task_id=?",{target.at("eventId"),task["id"]});
        if (!original.next()) return false;
        const auto question = original.json(0);
        const auto questionRun = question.value("run",Json::object());
        const auto targetRun = target.value("run",Json::object());
        return field(question,"kind") == "question" &&
            field(questionRun,"threadId") == field(targetRun,"threadId") &&
            field(questionRun,"runId") == field(targetRun,"runId") &&
            field(question.value("question",Json::object()),"requestId") == field(target,"requestId");
    }

    bool questionStillOpen(const Json& task) {
        const auto observed = observe(task);
        if (ingest(task,observed) || observed.kind != Observation::Kind::Question) return false;
        const auto target = questionTarget(task);
        const auto& question = task["question"];
        if (!question.is_object() || target["requestId"] != observed.question.requestId) return false;
        const auto& savedTarget = question.at("target");
        if (savedTarget == target) return true;
        auto forwardedTarget = target;
        forwardedTarget["eventId"] = savedTarget.at("eventId");
        return savedTarget == forwardedTarget && forwardedQuestionAnswer(task,savedTarget);
    }

    std::optional<TaskDecision> savedDecision(const Json& task, const Json& event) {
        auto saved = task.find("pendingDecision");
        if (saved == task.end() || saved->at("eventId") != event.at("id") ||
            saved->at("generation") != task.at("generation") || saved->at("revisionId") != revision(task))
            return std::nullopt;
        return TaskDecision{{saved->at("tool"),saved->at("args"),saved->at("reason")},saved->at("usage"),
                            saved->at("attemptId"),
                            saved->value("rejection",std::string{})};
    }

    std::optional<TaskDecision> persistTaskInference(Id id) {
        const auto found = taskInferences.find(id);
        const auto& inference = found->second;
        Transaction transaction(db.db);
        auto current = db.load(id);
        if (!inference.usage.empty())
            db.exec("UPDATE model_attempts SET usage=? WHERE id=?",{inference.usage,inference.attemptId});
        std::optional<TaskDecision> decision;
        if (current.at("state") == "deciding" && current.at("generation") == inference.generation &&
            current.at("eventId") == inference.eventId && revision(current) == inference.revisionId) {
            if (const auto* error = std::get_if<ModelError>(&inference.outcome)) {
                recordFailure(current,db.currentEvent(current),error->what(),error->transient);
                saveTask(current);
            } else {
                const auto& call = std::get<ModelCall>(inference.outcome);
                decision = TaskDecision{call,inference.usage,inference.attemptId,call.rejection};
                // Bookkeeping must not advance the generation observed by conversation turns.
                Json saved{{"generation",inference.generation},{"eventId",inference.eventId},{"revisionId",inference.revisionId},
                    {"tool",call.tool},{"args",call.args},{"reason",call.reason},{"usage",inference.usage},
                    {"attemptId",inference.attemptId}};
                if (!call.rejection.empty()) saved["rejection"] = call.rejection;
                db.exec("UPDATE tasks SET data=json_set(data,'$.pendingDecision',json(?)) WHERE id=?",{saved,id});
            }
        }
        transaction.commit();
        taskInferences.erase(found);
        return decision;
    }

    void persistTaskInferences() {
        std::lock_guard lock(db.mutex);
        while (!taskInferences.empty()) persistTaskInference(taskInferences.begin()->first);
    }

    std::optional<TaskDecision> inferDecision(const Json& task, Json event, const Json& history,
            const std::vector<std::string>& tools, const Json& pending, Json choices,
            const std::optional<PeerExchange>& peer) {
        const auto prefill = prefillHistory(task,event);
        if (prefill == PrefillResult::Persisted) return std::nullopt;
        if (prefill == PrefillResult::UnavailableWithSettledReport) event["historyUnavailable"] = true;
        const auto bot = db.loadNotes(field(task,"botId"),soul,options.clock());
        Json summary = {{"id",task["id"]},{"botId",task["botId"]},{"request",taskIntent(task)},
            {"doneWhen",task["doneWhen"]},
            {"remaining",task["remaining"]},{"additionalUsed",task["additionalUsed"]},{"revisionId",revision(task)}};
        if (task.contains("runTargets")) {
            summary["runTargets"] = task.at("runTargets");
            summary["requestedCommit"] = task.at("requestedCommit");
        }
        if (task.contains("sourceMessageId")) summary["sourceMessageId"] = task.at("sourceMessageId");
        if (task.contains("selectedSession") && !task.value("newSessionRequired",false)) summary["usesSelectedSession"] = true;
        if (task.contains("requestedDoneWhen")) summary["requestedDoneWhen"] = task.at("requestedDoneWhen");
        if (task.contains("restartContext")) summary["restartContext"] = task.at("restartContext");
        if (hasPending(task)) {
            summary["userInstructions"] = pending;
            summary["pendingInstructionCount"] = task.at("pendingInstructions").size();
        }
        if (task.contains("trigger")) summary["trigger"] = task.at("trigger");
        if (task.contains("sourceResultRefs")) summary["sourceResultRefs"] = task["sourceResultRefs"];
        if (!choices.empty()) summary["peers"] = std::move(choices);
        if (std::ranges::find(tools,"send_bot_message") != tools.end()) {
            std::lock_guard lock(db.mutex);
            auto forwarded = forwardableUserMessage(task);
            if (!forwarded.is_null()) summary["forwardableUserMessage"] = std::move(forwarded);
        }
        if (peer) summary["peer"] = {{"exchangeId",peer->id},{"requestKind",peer->kind},
            {"replyKinds",replyKinds(task,*peer)},{"phase",peer->phase},{"role",peer->sender == task["id"] ? "sender" : "recipient"}};
        const auto evidence = event["evidence"];
        const bool canReadHistory = std::ranges::find(tools,"session_read") != tools.end() ||
                                    std::ranges::find(tools,"read_bot_evidence") != tools.end();
        summary["remainingEvidenceReads"] = canReadHistory ? maxEvidenceReads - evidence.size() : 0;
        event.erase("evidence");
        event.erase("consumed");
        Id attempt = 0;
        {
            std::lock_guard lock(db.mutex);
            Transaction transaction(db.db);
            if (shutdown.stop_requested()) return std::nullopt;
            auto current = db.load(task["id"]);
            if (current["generation"] != task["generation"] || current["eventId"] != event["id"] ||
                revision(current) != revision(task) || current["state"] != "deciding")
                return std::nullopt;
            attempt = db.takeReservedTaskCall(event["id"]);
            if (!attempt) attempt = db.reserveModelCall(db.taskModelBudget(current),"task_decision",0,task["id"],event["id"]);
            if (!attempt) {
                db.wait(current,"model_budget",{{"text",
                    "The model call limit is exhausted. Approve one more model call to continue."}});
                saveTask(current);
            }
            transaction.commit();
        }
        if (!attempt) return std::nullopt;
        if (options.gitHost && event.at("kind") == "result")
            db.projectTaskGitReads(*options.gitHost,task,event);
        TaskInference inference{task.at("generation"),event.at("id"),attempt,revision(task)};
        try {
            auto turn = db.callModel(field(task,"botId"),model,
                {summary,event,history,bot,tools,field(task,"feedback"),evidence,task["config"],
                 needsDoneWhen(task),summary.contains("forwardableUserMessage")});
            inference.usage = std::move(turn.usage);
            if (turn.calls.size() == 1) inference.outcome = std::move(turn.calls.front());
            else inference.outcome = ModelError("Return one task action",false);
        } catch (const ModelError& error) {
            inference.usage = error.usage;
            inference.outcome = error;
        }
        const auto id = task.at("id").get<Id>();
        std::lock_guard lock(db.mutex);
        taskInferences.insert_or_assign(id,std::move(inference));
        return persistTaskInference(id);
    }

    void decide(const Json& task) {
        if (!task["activeRun"].is_null() && !questionStillOpen(task)) return;
        if (shutdown.stop_requested()) return;
        Json event, history = Json::array();
        std::optional<PeerExchange> peer;
        std::optional<TaskDecision> decision;
        {
            std::lock_guard lock(db.mutex);
            event = db.currentEvent(task);
            if (event.is_null() || event["consumed"] == true || event["retryAt"].get<Seconds>() > options.clock()) return;
            peer = db.peerExchange(task["id"]);
            decision = savedDecision(task,event);
            if (event["kind"] != "result" && !task["lastRun"].is_null()) {
                auto report = settledSessionReport(task);
                if (!report.is_null()) event["sessionResult"] = std::move(report);
            }
            if (hasSettledRun(task) && event.at("evidence").size() < maxEvidenceReads) {
                auto views = options.sessionRead == "on_demand" ? Json::array({"messages","activity"}) : Json::array();
                if (reportRetry(task,event)) views.push_back("report");
                if (!views.empty()) event["sessionReadViews"] = std::move(views);
            }
            auto errors = db.query(R"SQL(
SELECT data FROM journal WHERE task_id=? AND kind='tool_error' AND json_extract(data,'$.eventId')=?
AND id>coalesce((SELECT max(id) FROM journal WHERE task_id=? AND kind='model_retry'),0) ORDER BY id
)SQL",{task["id"],event["id"],task["id"]});
            event["decisionFeedback"] = Json::array();
            while (errors.next()) event["decisionFeedback"].push_back(errors.json(0));
            if (!decision) {
                auto rows = db.query(R"SQL(
SELECT id,kind,data,json_extract(data,'$.run.threadId'),json_extract(data,'$.resultRunId'),
       json_extract(data,'$.itemId'),coalesce(json_extract(data,'$.environmentId'),json_extract(data,'$.run.environmentId')) FROM journal WHERE task_id=?
AND kind IN ('decision','report','user_answer','session_answered','bot_message') ORDER BY id DESC LIMIT 6
)SQL",{task["id"]});
                std::size_t remaining = 16000;
                while (rows.next() && remaining > 100) {
                    const auto data = rows.string(2);
                    const auto excerpt = bounded(data,std::min<std::size_t>(remaining,5000));
                    if (data.size() == excerpt.size()) history.push_back({{"id",rows.number(0)},{"kind",rows.string(1)},{"data",Json::parse(data)}});
                    else history.push_back({{"id",rows.number(0)},{"kind",rows.string(1)},{"excerpt",excerpt},{"truncated",true},
                        {"source",{{"environmentId",rows.string(6)},{"threadId",rows.string(3)},
                            {"runId",rows.string(4)},{"itemId",rows.string(5)}}}});
                    remaining -= excerpt.size();
                }
                std::reverse(history.begin(),history.end());
            }
        }
        try {
            auto choices = Json::array();
            bool canRead = false;
            if (!peer && task["activeRun"].is_null() && field(task,"actionId").empty()) {
                for (const auto& allowed : peersFor(field(task,"botId"))) {
                    choices.push_back({{"botId",allowed.intake.botId},{"name",allowed.name}});
                    if (event.contains("references") && !event["references"].empty() && event["fromBotId"] == allowed.intake.botId)
                        canRead = true;
                }
            }
            Json pending = Json::array();
            if (hasPending(task)) {
                std::lock_guard lock(db.mutex);
                pending = workPrompt(task,event,0).pending;
            }
            auto tools = available(task,event,peer,!choices.empty(),canRead);
            if (options.gitHost && task["activeRun"].is_null() && event["kind"] == "result")
                tools.insert(tools.end(),gitTools(false).begin(),gitTools(false).end());
            if (hasPending(task) && task.at("pendingInstructions").size() != pending.size())
                std::erase(tools,"complete");
            const bool replay = decision.has_value();
            if (!decision) decision = inferDecision(task,event,history,tools,pending,std::move(choices),peer);
            if (!decision || shutdown.stop_requested()) return;
            if (!task["activeRun"].is_null() && !questionStillOpen(task)) return;
            if (!decision->rejection.empty()) {
                rejectDecision(task,event,*decision,decision->rejection);
                return;
            }
            if (replay) {
                auto call = db.checkModelCall(field(task,"botId"),*decision,tools,false);
                if (!call.rejection.empty()) {
                    rejectDecision(task,event,TaskDecision{std::move(call),decision->usage,decision->attemptId},call.rejection,true);
                    return;
                }
                decision->args = std::move(call.args);
                decision->reason = std::move(call.reason);
            }
            try {
                if (!decision->args.is_object() || decision->args.dump().size() > 20000 || !present(decision->reason,4000) ||
                    std::ranges::find(tools,decision->tool) == tools.end())
                    throw Conflict("Return exactly one permitted action, valid arguments and a reason");
                accept(task,event,*decision);
            } catch (const Conflict& error) {
                rejectDecision(task,event,*decision,error.what());
            }
        } catch (const ModelError& e) {
            failure(task,event,e.what(),e.transient);
        } catch (const StorageError&) {
            throw;
        } catch (const std::exception& e) {
            failure(task,event,e.what(),false);
        }
    }

    bool shouldStop(const Json& task) {
        if (db.replanReady(task)) return true;
        if (task["state"] != "cancelling") return false;
        std::lock_guard lock(db.mutex);
        const auto peer = db.peerExchange(task["id"]);
        return !peer || peer->recipient == task["id"];
    }

    bool continueStage(const Json& task) {
        if ((!task.contains("workflowId") && !task.value("freshContextRequested",false)) ||
            !hasSettledRun(task) || !hasPending(task)) return false;
        return change(task,[&](Json& current) {
            auto event = db.currentEvent(current);
            if (!event.is_null() && event.at("consumed") != true &&
                (event.at("kind") != "result" || event.contains("userAnswer"))) return false;
            if (event.is_null()) event = Json::object();
            const auto tool = current.value("newSessionRequired",false) ? "delegate" : "follow_up";
            const TaskDecision decision{{tool,Json::object(),"Apply accepted user instructions after the assigned Run settled"}};
            bool charge = false;
            if (!admitWork(current,decision,charge)) return true;
            const auto action = actionId();
            if (event.contains("id")) recordDecision(current,event,decision,action);
            consumeEvent(current);
            queueAction(current,action,tool,"Continue this Flock task.",decision.args,event,charge);
            return true;
        });
    }

    bool resumeGitApproval(const Json& task) {
        Json saved, event;
        {
            std::lock_guard lock(db.mutex);
            auto row = db.query("SELECT resume FROM policy_approvals WHERE task=? AND generation=?"
                                " AND turn=0 AND resume IS NOT NULL AND status<>'pending' ORDER BY id LIMIT 1",
                                {task.at("id"),task.at("generation")});
            if (!row.next()) return false;
            saved = row.json(0);
            event = db.currentEvent(task);
        }
        if (!options.gitHost || event.is_null()) return false;
        const TaskDecision decision{{field(saved,"tool"),saved.at("args"),field(saved,"reason")}};
        accept(task,event,decision,field(saved,"invocation"));
        return true;
    }

    void step(const Work& work) {
        if (shutdown.stop_requested()) return;
        const auto& [id,action] = work;
        auto task = snapshot(id);
        const auto state = field(task,"state");
        if (!action.empty()) {
            auto& run = verificationRun(task,action);
            if (verificationRunOutstanding(run)) stepCheck(task,run);
            return;
        }
        if (task.contains("runResults")) return;
        if (task.contains("pendingStageReport"))
            change(task,[&](Json& current) { return db.settleStageReport(current); });
        else if (shouldStop(task)) stop(task);
        else if (task.contains("pendingRunReport")) recoverRunReport(task);
        else if (state == "queued") activate(task);
        else if (state == "deciding") {
            if (!resumeGitApproval(task) && !continueStage(task)) decide(task);
        } else {
            if (!task["activeRun"].is_null() && (state == "running" || state == "waiting") &&
                ingest(task,observe(task))) return;
            const bool reconcile = state == "waiting" &&
                (task.contains("workflowId") || task.contains("selectedSession")) &&
                !field(task,"actionId").empty();
            if (state != "sending" && !reconcile) return;
            Json action;
            {
                std::lock_guard lock(db.mutex);
                action = db.outbox(task);
            }
            if (action.is_null()) return;
            if (reconcile) {
                const auto now = options.clock();
                const bool selectedStart = field(action,"tool") == "start_in_session";
                if (!selectedStart && !task.contains("workflowId")) return;
                const auto key = selectedStart ? "sessionReconcileAt" : "workflowReconcileAt";
                if (action.at("phase")!="started" || now<task.value(key,Seconds{})) return;
                if (!change(task,[&](Json& current) { current[key] = now+5; return true; })) return;
            }
            deliver(task,action);
        }
    }
};

Runtime::Runtime(Store& store, Model& model, T3& t3, SoulLoader soul, RuntimeOptions options)
    : p_(std::make_unique<Impl>(store,model,t3,std::move(soul),std::move(options))) {}
Runtime::~Runtime() = default;
void Runtime::requestStop() { p_->shutdown.request_stop(); }
void Runtime::runConversations() {
    if (p_->shutdown.stop_requested()) return;
    p_->settleApprovals();
    p_->db.persistWorkspaceResults();
    p_->db.deliverGitWebhooks();
    p_->db.settleSessionQueueCancellations(p_->t3,p_->shutdown.get_token());
    p_->db.wakeSessionMessages(p_->t3,p_->shutdown.get_token());
    p_->db.deliverChannels(p_->shutdown.get_token());
    constexpr auto messagesPerTick = 20;
    for (int i = 0; i < messagesPerTick && !p_->shutdown.stop_requested(); ++i) {
        const auto pending = p_->db.pendingConversations(1,p_->options.clock());
        if (pending.empty()) break;
        p_->runTurn(pending.front().second);
    }
    p_->db.deliverChannels(p_->shutdown.get_token());
}
void Runtime::tick() {
    if (p_->shutdown.stop_requested()) return;
    p_->settleApprovals();
    p_->persistTaskInferences();
    p_->db.persistWorkspaceResults();
    p_->fireDueAutomations();
    p_->db.deliverGitWebhooks();
    p_->db.settleSessionQueueCancellations(p_->t3,p_->shutdown.get_token());
    p_->db.wakeSessionMessages(p_->t3,p_->shutdown.get_token());
    for (const auto& [bot,id] : p_->db.pendingConversations(Impl::batchLimit,p_->options.clock()))
        p_->runTurn(id);
    p_->db.advanceWorkflows(p_->options.clock());
    for (const auto& work : p_->nextProjects(Impl::batchLimit)) p_->step(work);
    p_->db.deliverChannels(p_->shutdown.get_token());
}
void Runtime::tickConcurrent() {
    if (p_->shutdown.stop_requested()) return;
    p_->deferContendedWork("policy approvals",[&] { p_->settleApprovals(); });
    const auto collect = [this](auto& workers, std::string_view operation) {
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto result = std::move(it->second);
                it = workers.erase(it);
                p_->deferContendedWork(operation,[&] { result.get(); });
            } else ++it;
        }
    };
    collect(p_->conversationWorkers,"conversation worker");
    collect(p_->workers,"task worker");
    if (p_->channelWorker.valid() &&
        p_->channelWorker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        p_->deferContendedWork("channel worker",[&] { p_->channelWorker.get(); });
    p_->deferContendedWork("workspace outcomes",[&] { p_->db.persistWorkspaceResults(); });
    p_->deferContendedWork("automations",[&] { p_->fireDueAutomations(); });
    p_->deferContendedWork("Git webhooks",[&] { p_->db.deliverGitWebhooks(); });
    if (!p_->channelWorker.valid() && p_->db.channelDeliveryDue())
        p_->channelWorker = std::async(std::launch::async,[db = &p_->db,stop = p_->shutdown.get_token()] {
            db->deliverChannels(stop);
        });
    if (p_->sessionWakeWorker.valid() &&
        p_->sessionWakeWorker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        p_->deferContendedWork("Session wake worker",[&] { p_->sessionWakeWorker.get(); });
    if (!p_->sessionWakeWorker.valid())
        p_->sessionWakeWorker = std::async(std::launch::async,[impl = p_.get()] {
            impl->db.settleSessionQueueCancellations(impl->t3,impl->shutdown.get_token());
            impl->db.wakeSessionMessages(impl->t3,impl->shutdown.get_token());
        });
    constexpr auto maxConversationWorkers = Impl::batchLimit;
    if (p_->conversationWorkers.size() < maxConversationWorkers) {
        for (const auto& [bot,id] : p_->db.pendingConversations(maxConversationWorkers,p_->options.clock())) {
            if (p_->conversationWorkers.size() >= maxConversationWorkers) break;
            if (!p_->conversationWorkers.contains(bot)) {
                p_->conversationWorkers.emplace(bot,std::async(std::launch::async,[impl = p_.get(),id] { impl->runTurn(id); }));
            }
        }
    }
    p_->deferContendedWork("workflows",[&] { p_->db.advanceWorkflows(p_->options.clock()); });
    p_->deferContendedWork("task scheduling",[&] {
        p_->persistTaskInferences();
        for (const auto& work : p_->nextProjects(Impl::batchLimit - p_->workers.size()))
            p_->workers.emplace(work,std::async(std::launch::async,[impl = p_.get(),work] { impl->step(work); }));
    });
}
} // namespace flockv0
