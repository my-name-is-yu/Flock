#include "storage.hpp"

#include <algorithm>
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

std::string intent(const Json& task) {
    return task.value("intent",field(task,"request"));
}

Json revision(const Json& task) { return task.value("currentRevision",Json(nullptr)); }

bool hasPending(const Json& task) {
    const auto pending = task.find("pendingInstructions");
    return pending != task.end() && !pending->empty();
}

std::string replacementKey(const Json& item) {
    if (!item.value("conflicting",false)) return {};
    const auto key = field(item,"replaceKey");
    if (!key.empty()) return key;
    return field(item,"environmentId").empty() ? "target" : "environment";
}

struct InstructionBatch {
    Json instructions = Json::array(), superseded = Json::array();
};

InstructionBatch pendingBatch(const Json& task) {
    InstructionBatch batch;
    if (!hasPending(task)) return batch;
    const auto& pending = task["pendingInstructions"];
    std::map<std::string,std::size_t> latestOverrideIndices;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        if (auto key = replacementKey(pending[i]); !key.empty()) latestOverrideIndices[std::move(key)] = i;
    }
    std::size_t reservedOverrideBytes = 0;
    for (const auto& [key,i] : latestOverrideIndices) reservedOverrideBytes += field(pending[i],"text").size() + 4;
    auto batchBytes = reservedOverrideBytes;
    bool acceptsIndependentInstructions = true;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& item = pending[i];
        if (const auto key = replacementKey(item); !key.empty()) {
            if (latestOverrideIndices.at(key) == i) batch.instructions.push_back(item);
            else batch.superseded.push_back(item);
        } else if (acceptsIndependentInstructions) {
            const auto size = field(item,"text").size() + 4;
            if (batchBytes + size <= 64000) {
                batch.instructions.push_back(item);
                batchBytes += size;
            } else acceptsIndependentInstructions = false;
        }
    }
    return batch;
}

void includeInstructions(std::string& instruction, const Json& pending) {
    if (pending.empty()) return;
    instruction += "\n\nUser instructions accepted for this task, oldest first. Apply later directions where they replace earlier ones:";
    for (const auto& item : pending) instruction += "\n\n" + field(item,"text");
}

void validateAnswers(const Json& fields, const Json& answers) {
    if (!fields.is_array() || fields.empty() || !answers.is_object())
        throw Conflict("Answers are required per question field");
    std::set<std::string> ids;
    for (const auto& f : fields) {
        const auto id = field(f,"id");
        if (id.empty() || !ids.insert(id).second) throw Conflict("Unsupported question fields");
        const bool required = f.value("required",true);
        if (!answers.contains(id)) {
            if (required) throw Conflict("A required question field was not answered");
            continue;
        }
        const auto& a = answers[id];
        const auto values = a.is_string() ? Json::array({a}) : a;
        if (!values.is_array() || (required && values.empty()) ||
            (values.size() > 1 && !f.value("multiple",false)))
            throw Conflict("Question field answer has the wrong cardinality");
        const auto options = f.value("options",Json::array());
        const bool freeform = f.value("freeform",options.empty());
        std::set<std::string> seen;
        for (const auto& v : values) {
            if (!v.is_string() || !present(v.get<std::string>()) || !seen.insert(v.get<std::string>()).second)
                throw Conflict("Invalid or repeated answer");
            if (!freeform && std::none_of(options.begin(),options.end(),[&](const Json& option) {
                return option == v || field(option,"value") == v.get<std::string>();
            })) throw Conflict("Answer does not match a question option");
        }
    }
    for (auto i = answers.begin(); i != answers.end(); ++i)
        if (!ids.contains(i.key())) throw Conflict("Answer references an unknown question field");
}
}

struct Runtime::Impl {
    Store::Impl& db;
    Access access;
    Model& model;
    T3& t3;
    NotesLoader notes;
    RuntimeOptions options;
    int lockFd = -1;
    std::map<Id,std::future<void>> workers;
    struct HistoryUnavailable : ModelError { using ModelError::ModelError; };
    std::map<std::string,std::future<void>> conversationWorkers;

    Impl(Store& store, Model& m, T3& adapter, NotesLoader loader, RuntimeOptions opts)
        : db(*store.p_), access(store.access()), model(m), t3(adapter),
          notes([load = std::move(loader)](std::string_view botId) {
              constexpr auto limit = 8192;
              auto bot = load(botId);
              bot.soul = bounded(bot.soul,limit);
              bot.memory = bounded(bot.memory,limit);
              return bot;
          }), options(std::move(opts)) {
        if (!options.botTools) options.botTools = std::make_shared<BotTools>();
        const auto lockPath = std::filesystem::canonical(db.path).string() + ".runtime.lock";
        lockFd = open(lockPath.c_str(), O_CLOEXEC | O_CREAT | O_RDWR, 0600);
        if (lockFd < 0 || ::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
            if (lockFd >= 0) close(lockFd);
            throw Conflict("Cannot acquire the exclusive Runtime lock for this database");
        }
        try { db.recoverWorkspace(); }
        catch (...) { close(lockFd); throw; }
    }
    ~Impl() {
        conversationWorkers.clear();
        workers.clear();
        db.stopWorkspace();
        close(lockFd);
    }

    template<class F> bool changeTask(Id id, F fn) {
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        auto task = db.load(id);
        if (!fn(task)) return false;
        db.save(task);
        transaction.commit();
        return true;
    }

    template<class F> bool change(const Json& snapshot, F fn) {
        return changeTask(snapshot["id"],[&](Json& task) {
            return task["generation"] == snapshot["generation"] && fn(task);
        });
    }

    void clearDeferredCompletionMarker(Id taskId, Id eventId) {
        if (!eventId) return;
        auto row = db.query("SELECT data FROM events WHERE id=? AND task_id=? AND consumed=0",{eventId,taskId});
        if (!row.next()) return;
        auto data = row.json(0);
        const auto saved = data.erase("_flockSavedCompletion");
        if (data.erase("_flockDeferredCompletion") || saved)
            db.exec("UPDATE events SET data=? WHERE id=? AND task_id=? AND consumed=0",{data,eventId,taskId});
    }

    std::vector<Id> eligible() {
        std::lock_guard lock(db.mutex);
        auto rows = db.query(R"SQL(
SELECT id FROM project_execution_queue WHERE position=1
UNION
SELECT id FROM tasks WHERE state NOT IN ('completed','cancelled') AND (
  COALESCE(json_extract(data,'$.environmentReleased'),0)=1 OR
  (state='queued' AND json_extract(data,'$.replanning')=1 AND json_extract(data,'$.pendingEnvironmentId')<>environment_id)
)
UNION
SELECT tasks.id FROM tasks JOIN peer_exchanges ON tasks.id=recipient_task
WHERE kind='consult' AND state NOT IN ('completed','cancelled')
UNION
SELECT tasks.id FROM tasks JOIN peer_exchanges ON tasks.id=recipient_task
WHERE state='cancelling' AND json_extract(tasks.data,'$.activeRun') IS NULL AND NOT EXISTS (
  SELECT 1 FROM outbox WHERE outbox.id=json_extract(tasks.data,'$.actionId') AND phase='started'
)
ORDER BY id)SQL");
        std::vector<Id> result;
        while (rows.next()) result.push_back(rows.number(0));
        return result;
    }
    Json snapshot(Id id) {
        std::lock_guard lock(db.mutex);
        return db.load(id);
    }

    void activate(const Json& task) {
        change(task,[&](Json& current) {
            auto turn = db.query("SELECT 1 FROM project_execution_queue WHERE id=? AND position=1"
                                 " UNION ALL SELECT 1 FROM peer_exchanges WHERE recipient_task=? AND kind='consult'",
                                 {current["id"],current["id"]});
            if (!turn.next()) return false;
            if (current["eventId"] == 0) db.event(current,"intake",{{"kind","intake"}});
            current["state"] = "deciding";
            return true;
        });
    }

    void cancelDone(Json& task) {
        clearDeferredCompletionMarker(task["id"].get<Id>(),task.value("eventId",Id{0}));
        db.clearWait(task,"cancelled");
        db.unlockQuestion(task,"cancelled");
        if (!field(task,"actionId").empty()) {
            db.exec("UPDATE outbox SET phase='closed' WHERE id=? AND phase='ready'", {task["actionId"]});
            task["actionId"] = "";
        }
        if (const auto stop = field(task,"stopActionId"); !stop.empty()) {
            db.exec("UPDATE outbox SET phase='closed' WHERE id=? AND phase IN ('ready','started')", {stop});
            task.erase("stopActionId");
        }
        task.erase("replanning");
        task.erase("pendingEnvironmentId");
        task.erase("environmentReleased");
        task.erase("pendingInstructions");
        task["activeRun"] = nullptr;
        task["state"] = "cancelled";
        db.journal(task["id"],"cancelled",Json::object());
        if (const auto peer = db.peerExchange(task["id"]); peer && peer->recipient == task["id"])
            finishPeer(*peer,task,"cancelled","The receiving Bot's task was cancelled.",actionId());
    }

    void cancellationNote(const Json& snapshot, const std::string& detail) {
        change(snapshot,[&](Json& task) {
            if (field(task,"cancellationDetail") == detail) return false;
            task["cancellationDetail"] = detail;
            db.journal(task["id"],"cancelling",{{"detail",detail},{"run",task["activeRun"]}});
            return true;
        });
    }
    bool ingest(const Json& snapshot, const Observation& observed) {
        return change(snapshot,[&](Json& task) {
            const auto run = runRef(task["activeRun"]);
            const auto state = field(task,"state");
            const auto reason = field(task["wait"],"reason");
            const bool replanning = task.value("replanning",false);
            const bool settledRunReleasesStopBarrier =
                observed.kind == Observation::Kind::Finished && observed.settled;
            if (settledRunReleasesStopBarrier) {
                db.clearWait(task,"run_finished");
                db.unlockQuestion(task,"run_finished");
                // An ended Run cannot accept an outstanding reply. Preserve the
                // action in history while letting the definitive Run result take precedence.
                if (!field(task,"actionId").empty()) {
                    const auto action = db.outbox(task);
                    if (!action.is_null() && action["tool"] == "answer_session_question") {
                        db.exec("UPDATE outbox SET phase='closed' WHERE id=?", {task["actionId"]});
                        task["actionId"] = "";
                    }
                }
                task["lastRun"] = task["activeRun"];
                task["activeRun"] = nullptr;
                db.consumeEvent(task);
                const auto report = bounded(observed.report,8192);
                auto work = db.query(R"SQL(
SELECT outbox.data FROM runs JOIN outbox ON runs.action_id=outbox.id
WHERE runs.task_id=? AND runs.environment_id=? AND runs.thread_id=? AND runs.run_id=?
)SQL", {task["id"],task["environmentId"],run.threadId,run.runId});
                const auto assignment = work.next() ? work.json(0) : Json::object();
                const Json data = {{"kind","result"},{"run",task["lastRun"]},
                    {"resultRunId",observed.resultRunId.empty() ? run.runId : observed.resultRunId},
                    {"status",observed.status},{"detail",bounded(observed.detail,2048)},
                    {"report",report},{"truncated",observed.truncated || report.size() < observed.report.size()},
                    {"environmentId",task["environmentId"]},{"itemId",observed.itemId},
                    {"revisionId",assignment.value("revisionId",Json(nullptr))}};
                db.exec("INSERT OR IGNORE INTO t3_events VALUES(?,?,'result',?)",{task["environmentId"],run.threadId,run.runId});
                if (sqlite3_changes(db.db)) {
                    if (state != "cancelling" && !replanning) {
                        task.erase("feedback");
                        db.event(task,"result/" + field(task,"environmentId") + "/" + run.threadId + "/" + run.runId,data);
                    }
                    db.journal(task["id"],"report",data);
                }
                if (const auto stop = field(task,"stopActionId"); !stop.empty()) {
                    db.exec("UPDATE outbox SET phase='accepted' WHERE id=?", {stop});
                    db.journal(task["id"],"interrupt_settled",{{"actionId",stop},{"run",task["lastRun"]}});
                    task.erase("stopActionId");
                    task.erase("replanInterruptAt");
                }
                if (state == "cancelling") cancelDone(task);
                else task["state"] = "deciding";
                return true;
            }
            if (state == "cancelling" || replanning) return false;
            if (!field(task,"actionId").empty()) return false;
            if (observed.kind == Observation::Kind::Question) {
                if (observed.question.requestId.empty()) return false;
                if (field(task["question"],"requestId") == observed.question.requestId) return false;
                db.exec("INSERT OR IGNORE INTO t3_events VALUES(?,?,'question',?)",
                        {task["environmentId"],run.threadId,observed.question.requestId});
                if (!sqlite3_changes(db.db)) return false;
                db.clearWait(task,"session_question");
                db.unlockQuestion(task,"replaced");
                db.consumeEvent(task);
                task.erase("feedback");
                const Json question = {{"requestId",observed.question.requestId},{"fields",observed.question.fields}};
                task["question"] = question;
                const Json data = {{"kind","question"},{"run",runJson(run)},{"question",question}};
                db.event(task,"question/" + field(task,"environmentId") + "/" + run.threadId + "/" + observed.question.requestId,data);
                db.journal(task["id"],"session_question",data);
                task["state"] = "deciding";
                return true;
            }
            if (observed.kind == Observation::Kind::ExternalInput) {
                if (reason == "t3_input") return false;
                db.unlockQuestion(task,"requires_t3");
                db.consumeEvent(task);
                db.wait(task,"t3_input",{{"text",bounded(observed.status,2048)}});
                return true;
            }
            if (observed.kind == Observation::Kind::Unavailable) {
                if (observed.status == "auth" && reason != "auth") {
                    db.wait(task,"auth",{{"text","T3 authentication is unavailable"}});
                    return true;
                }
                if (observed.status == "settings" && reason != "settings") {
                    db.wait(task,"settings",{{"text","T3 connection settings need attention"}});
                    return true;
                }
            }
            const bool available = observed.kind != Observation::Kind::Unavailable;
            if (available && (reason == "t3_input" || !task["question"].is_null())) {
                db.clearWait(task,"session_question_resolved");
                db.unlockQuestion(task,"resolved_in_t3");
                db.consumeEvent(task);
                task["state"] = "running";
                return true;
            }
            if (state == "running" && task["deadline"].get<Seconds>() > 0 &&
                options.clock() >= task["deadline"].get<Seconds>()) {
                db.wait(task,"deadline",{{"text","The assigned Run reached its deadline"}});
                return true;
            }
            return false;
        });
    }

    Observation observe(const Json& task) {
        try {
            auto observed = t3.observe(field(task,"environmentId"),task["config"],runRef(task["activeRun"]));
            const auto scrub = db.redactor(field(task,"botId"));
            scrub.requirePublic(observed.question.requestId);
            if (scrub.text(observed.resultRunId)!=observed.resultRunId) observed.resultRunId.clear();
            if (scrub.text(observed.itemId)!=observed.itemId) observed.itemId.clear();
            observed.question.fields = scrub.payload(observed.question.fields);
            observed.report = scrub.text(observed.report);
            observed.detail = scrub.text(observed.detail);
            if (observed.status!="auth" && observed.status!="completed" && observed.status!="failed" &&
                observed.status!="cancelled" && observed.status!="interrupted" && observed.status!="rolled_back")
                observed.status = scrub.text(observed.status);
            return observed;
        } catch (const std::exception&) {
            Observation observation;
            observation.kind = Observation::Kind::Unavailable;
            return observation;
        }
    }

    void cancel(const Json& task) {
        Json action;
        {
            std::lock_guard lock(db.mutex);
            const auto peer = db.peerExchange(task["id"]);
            if (peer && peer->sender == task["id"]) return;
            action = db.outbox(task);
        }
        if (!action.is_null() && action["phase"] == "started") {
            const auto now = options.clock();
            if (now < task.value("cancelReconcileAt",Seconds{0})) {
                if (action["tool"] == "answer_session_question" && !task["activeRun"].is_null()) cancelRun(task);
                return;
            }
            if (!change(task,[&](Json& current) { current["cancelReconcileAt"] = now + 3; return true; })) return;
            deliver(task,action);
            if (action["tool"] != "answer_session_question" || task["activeRun"].is_null()) return;
            // A lost answer acknowledgement does not prevent stopping its known Run.
            return cancelRun(snapshot(task["id"]));
        }
        if (task["activeRun"].is_null()) {
            change(task,[&](Json& current) { cancelDone(current); return true; });
            return;
        }
        cancelRun(task);
    }
    void cancelRun(const Json& task) {
        const auto observed = observe(task);
        if (ingest(task,observed)) return;
        if (observed.kind == Observation::Kind::Finished) {
            cancellationNote(task,"Waiting for the assigned Run's delegated work to settle");
            return;
        }
        if (observed.kind == Observation::Kind::Unavailable) {
            cancellationNote(task,"Cannot locate or observe the assigned Run");
            return;
        }
        const auto now = options.clock();
        if (task.contains("interruptAt") && now - task["interruptAt"].get<Seconds>() < 5) return;
        if (!change(task,[&](Json& current) {
            current["interruptAt"] = now;
            db.journal(current["id"],"interrupt_requested",{{"run",current["activeRun"]}});
            return true;
        })) return;
        try { t3.interrupt(field(task,"environmentId"),task["config"],runRef(task["activeRun"])); }
        catch (const std::exception&) { cancellationNote(snapshot(task["id"]),"T3 did not confirm the interrupt request"); }
    }

    void replanNote(const Json& task, const std::string& detail) {
        change(task,[&](Json& current) {
            if (field(current,"replanningDetail") == detail) return false;
            current["replanningDetail"] = detail;
            db.journal(current["id"],"replanning",{{"detail",detail},{"run",current["activeRun"]}});
            return true;
        });
    }

    void stopForRevision(const Json& task) {
        if (task["state"] == "cancelling" || task["activeRun"].is_null()) return;
        const auto stop = field(task,"stopActionId");
        if (stop.empty()) {
            change(task,[&](Json& current) {
                const auto id = actionId();
                const Json out = {{"id",id},{"tool","interrupt"},{"environmentId",current["environmentId"]},
                    {"target",current["activeRun"]},{"revisionId",revision(current)}};
                db.exec("INSERT INTO outbox(id,task_id,phase,data) VALUES(?,?,'ready',?)", {id,current["id"],out});
                current["stopActionId"] = id;
                db.journal(current["id"],"interrupt_queued",{{"actionId",id},{"run",current["activeRun"]}});
                return true;
            });
            return;
        }
        const auto observed = observe(task);
        if (ingest(task,observed)) return;
        if (observed.kind == Observation::Kind::Finished) {
            replanNote(task,"Waiting for delegated work to settle");
            return;
        }
        if (observed.kind == Observation::Kind::Unavailable) {
            replanNote(task,"Cannot confirm that the prior Run has stopped");
            return;
        }
        const auto now = options.clock();
        if (now < task.value("replanInterruptAt",Seconds{0})) return;
        if (!change(task,[&](Json& current) {
            auto saved = db.query("SELECT data FROM outbox WHERE id=? AND task_id=? AND phase IN ('ready','started')",
                                  {stop,current["id"]});
            if (!saved.next() || saved.json(0)["target"] != current["activeRun"]) return false;
            db.exec("UPDATE outbox SET phase='started' WHERE id=?", {stop});
            current["replanInterruptAt"] = now + 5;
            db.journal(current["id"],"interrupt_requested",{{"actionId",stop},{"run",current["activeRun"]}});
            return true;
        })) return;
        try { t3.interrupt(field(task,"environmentId"),task["config"],runRef(task["activeRun"])); }
        catch (const std::exception&) { replanNote(snapshot(task["id"]),"T3 did not confirm the interrupt request"); }
    }

    void finishReplan(const Json& task) {
        change(task,[&](Json& current) {
            const auto old = field(current,"environmentId");
            auto target = field(current,"pendingEnvironmentId");
            if (target.empty()) target = old;
            const auto id = current["id"].get<Id>();
            const auto peer = db.peerExchange(id);
            const bool workingRecipient = peer && peer->recipient == id && peer->kind == "work" && peer->phase == "working";
            if (peer && !workingRecipient) return false;
            const auto event = db.currentEvent(current);
            const bool preservePeerAnswer = workingRecipient && !event.is_null() && event["consumed"] != true &&
                event["kind"] == "bot_answer";
            if (target != old && !current.value("environmentReleased",false)) {
                current["environmentReleased"] = true;
                return true;
            }
            if (target != old || current.value("environmentReleased",false) || current["state"] == "queued") {
                const auto blocker = db.queueBlocker(id);
                if (!blocker.is_null()) {
                    const bool waitingSender = workingRecipient && blocker["id"] == peer->sender &&
                        blocker["state"] == "peer_waiting";
                    if (!waitingSender) return false;
                }
            }
            if (target != old) {
                current["environmentId"] = target;
                current["newSessionRequired"] = true;
                db.journal(current["id"],"session_destination_changed",{{"from",old},{"to",target},{"revisionId",revision(current)}});
            }
            db.clearWait(current,"user_instruction");
            db.unlockQuestion(current,"user_instruction");
            if (!preservePeerAnswer) {
                clearDeferredCompletionMarker(id,current.value("eventId",Id{0}));
                db.consumeEvent(current);
                if (current["lastRun"].is_null())
                    db.event(current,"replan/" + std::to_string(current["generation"].get<Id>()),{{"kind","intake"}});
            }
            current["state"] = "deciding";
            for (const auto* key : {"replanning","replanningDetail","pendingEnvironmentId","environmentReleased","replanReconcileAt","feedback"})
                current.erase(key);
            return true;
        });
    }

    void replan(const Json& task) {
        Json action;
        {
            std::lock_guard lock(db.mutex);
            action = db.outbox(task);
        }
        if (!action.is_null() && action["phase"] == "started") {
            const auto now = options.clock();
            const bool reply = action["tool"] == "answer_session_question";
            if (now >= task.value("replanReconcileAt",Seconds{0}) && change(task,[&](Json& current) {
                current["replanReconcileAt"] = now + 3;
                return true;
            })) deliver(task,action);
            if (reply && !task["activeRun"].is_null()) stopForRevision(snapshot(task["id"]));
            return;
        }
        if (!action.is_null() && action["phase"] == "ready") {
            change(task,[&](Json& current) {
                db.exec("UPDATE outbox SET phase='superseded' WHERE id=? AND phase='ready'", {action["id"]});
                if (!sqlite3_changes(db.db)) return false;
                if (action["tool"] != "answer_session_question" && action.value("chargesBudget",!current["lastRun"].is_null())) {
                    current["remaining"] = current["remaining"].get<int>() + 1;
                    current["additionalUsed"] = current["additionalUsed"].get<int>() - 1;
                }
                current["actionId"] = "";
                db.clearWait(current,"superseded_instruction");
                db.journal(current["id"],"delivery_superseded",{{"actionId",action["id"]},{"revisionId",revision(current)}});
                return true;
            });
            return;
        }
        if (!task["activeRun"].is_null()) stopForRevision(task);
        else finishReplan(task);
    }

    Command command(const Json& task, const Json& action) {
        Command c;
        c.ownerBotId = field(task,"botId");
        c.actionId = field(action,"id");
        c.tool = field(action,"tool");
        c.environmentId = action.value("environmentId",field(task,"environmentId"));
        c.projectId = field(task,"projectId");
        c.instruction = field(action,"instruction");
        c.config = task["config"];
        c.target = runRef(action.value("target",Json::object()));
        c.questionId = field(action,"questionId");
        c.answers = action.value("answers",Json::object());
        c.authGeneration = field(action,"authGeneration");
        c.candidate = runRef(action.value("candidate",Json::object()));
        c.candidateQuestionId = field(action,"candidateQuestionId");
        return c;
    }

    void instructionDelivery(Json& task, const Json& action) {
        const auto instructions = action.value("instructions",Json::array());
        if (instructions.empty()) return;
        auto& pending = task["pendingInstructions"];
        for (const auto& item : instructions) {
            const auto copy = std::find(pending.begin(),pending.end(),item);
            if (copy != pending.end()) pending.erase(copy);
            db.journal(task["id"],"instruction_delivered",{{"actionId",action["id"]},{"messageId",item["messageId"]},
                {"step",item.value("step",0)},
                {"revisionId",action.value("revisionId",Json(nullptr))},{"run",task["activeRun"]},
                {"source",item.value("source",Json(nullptr))}});
        }
    }

    void supersedeInstructions(Json& task, const Json& items) {
        if (items.empty()) return;
        auto& pending = task["pendingInstructions"];
        for (const auto& item : items) {
            const auto copy = std::find(pending.begin(),pending.end(),item);
            if (copy != pending.end()) pending.erase(copy);
            db.journal(task["id"],"instruction_superseded",{{"messageId",item["messageId"]},{"step",item.value("step",0)},
                {"revisionId",item["revisionId"]}});
        }
    }

    bool assignedRunMatches(const Json& task, const Json& action, const RunRef& run) {
        auto owner = db.query("SELECT action_id FROM runs WHERE environment_id=? AND thread_id=? AND run_id=?",
                              {task["environmentId"],run.threadId,run.runId});
        if (owner.next() && owner.string(0) != field(action,"id")) return false;
        if (action["tool"] == "follow_up") {
            const auto target = runRef(action["target"]);
            if (run.threadId != target.threadId || run.runId == target.runId) return false;
            auto prior = db.query("SELECT 1 FROM runs WHERE task_id=? AND environment_id=? AND thread_id=? AND run_id=?",
                                  {task["id"],task["environmentId"],target.threadId,target.runId});
            return prior.next();
        }
        auto prior = db.query("SELECT 1 FROM runs WHERE environment_id=? AND thread_id=? AND action_id<>? LIMIT 1",
                              {task["environmentId"],run.threadId,action["id"]});
        return !prior.next();
    }

    void applyDelivery(Id id, const Json& action, Delivery outcome) {
        changeTask(id,[&](Json& task) {
            if (task["actionId"] != action["id"] || terminal(task)) return false;
            const auto scrub = db.redactor(field(task,"botId"));
            if (scrub.payload(runJson(outcome.run)) != runJson(outcome.run))
                outcome = {Delivery::Kind::Unknown,{},"T3 returned a protected Run identifier"};
            outcome.detail = scrub.text(outcome.detail);
            const bool cancelling = task["state"] == "cancelling";
            const bool reply = action["tool"] == "answer_session_question";
            if (outcome.kind == Delivery::Kind::Accepted && !reply && hasRun(outcome.run) &&
                !assignedRunMatches(task,action,outcome.run)) {
                outcome.kind = Delivery::Kind::Unknown;
                outcome.detail = "The returned Run does not match the assigned work";
                outcome.run = {};
            }
            if (outcome.kind == Delivery::Kind::Accepted && (reply || hasRun(outcome.run))) {
                db.exec("UPDATE outbox SET phase='accepted' WHERE id=?", {action["id"]});
                db.clearWait(task,"delivery_confirmed");
                if (reply) db.unlockQuestion(task,"answered");
                else {
                    db.exec("INSERT OR IGNORE INTO runs VALUES(?,?,?,?,?,?)", {action["id"],id,
                        task["environmentId"],outcome.run.threadId,outcome.run.runId,outcome.run.link});
                    task["activeRun"] = runJson(outcome.run);
                    task["activeRun"]["ownerBotId"] = task["botId"];
                    task["activeRun"]["environmentId"] = task["environmentId"];
                    task["activeRun"]["revisionId"] = action.value("revisionId",Json(nullptr));
                    db.journal(id,"run_started",{{"run",task["activeRun"]},{"actionId",action["id"]},
                        {"revisionId",task["activeRun"]["revisionId"]},
                        {"sourceResultRefs",action.value("sourceResultRefs",Json::array())}});
                    instructionDelivery(task,action);
                    if (action["tool"] == "delegate") task.erase("newSessionRequired");
                }
                if (reply) db.journal(id,"session_answered",{{"requestId", action["questionId"]},{"actionId",action["id"]}});
                task["actionId"] = "";
                if (!cancelling) task["state"] = "running";
                return true;
            }
            if (outcome.kind == Delivery::Kind::QuestionGone && reply) {
                db.exec("UPDATE outbox SET phase='closed' WHERE id=?", {action["id"]});
                db.clearWait(task,"question_gone");
                db.unlockQuestion(task,"resolved_or_expired");
                task["actionId"] = "";
                if (!cancelling) task["state"] = "running";
                return true;
            }
            if (outcome.kind == Delivery::Kind::Unknown && !outcome.run.threadId.empty() &&
                action["phase"] == "ready" && !reply && !cancelling) {
                auto saved = action;
                saved.erase("phase");
                saved.erase("authGeneration");
                saved["candidate"] = runJson(outcome.run);
                db.exec("UPDATE outbox SET data=? WHERE id=?", {saved,action["id"]});
                db.journal(id,"run_id_pending",{{"actionId",action["id"]},{"thread",runJson(outcome.run)}});
                return true;
            }
            const auto reason = outcome.kind == Delivery::Kind::Auth ? "auth" :
                                outcome.kind == Delivery::Kind::Settings ? "settings" : "unknown_delivery";
            const auto detail = outcome.detail.empty() ? "The assigned side effect could not be confirmed" :
                                                         bounded(outcome.detail,2048);
            const bool retryableDelivery = outcome.kind == Delivery::Kind::NotAccepted || outcome.definitelyUnsent;
            if (retryableDelivery) {
                db.exec("UPDATE outbox SET phase=? WHERE id=?", {cancelling ? "closed" : "ready",action["id"]});
                if (!reply) task["deadline"] = 0;
                if (cancelling) task["actionId"] = "";
            }
            if (cancelling) {
                if (field(task,"cancellationDetail") == detail && !retryableDelivery) return false;
                task["cancellationDetail"] = detail;
                db.journal(id,"cancelling",{{"detail",detail},{"actionId",action["id"]}});
            } else if (field(task["wait"],"reason") != reason || field(task["wait"],"text") != detail) {
                db.wait(task,reason,{{"text",detail}});
            } else if (!retryableDelivery) return false;
            return true;
        });
    }

    void deliver(const Json& task, const Json& action) {
        auto cmd = command(task,action);
        const auto id = task["id"].get<Id>();
        Delivery outcome;
        try {
            if (action["phase"] == "started") {
                outcome = t3.reconcile(cmd);
            } else {
                const auto launchAllowed = [&] {
                    return (cmd.tool != "delegate" && cmd.tool != "follow_up") ||
                           access.permits(cmd.ownerBotId,{"environment",cmd.environmentId,"launch"},true);
                };
                const Delivery denied{Delivery::Kind::Settings,{},"Approve launch access in /settings/"+cmd.ownerBotId+"/environment",true};
                if (!launchAllowed()) {
                    applyDelivery(id,action,denied);
                    return;
                }
                if (const auto stop = t3.preflight(cmd)) {
                    applyDelivery(id,action,*stop);
                    return;
                }
                cmd.authGeneration = t3.authGeneration(cmd.environmentId,cmd.config);
                if (!change(task,[&](Json& current) {
                    db.exec("UPDATE outbox SET phase='started',auth_generation=? WHERE id=?", {cmd.authGeneration,action["id"]});
                    if (cmd.tool != "answer_session_question" && current["deadline"] == 0)
                        current["deadline"] = options.clock() + db.environmentTimeout(cmd.ownerBotId,cmd.environmentId,current.at("config"));
                    db.journal(current["id"],"send_started",{{"actionId",action["id"]}});
                    return true;
                })) return;
                outcome = launchAllowed() ? t3.send(cmd) : denied;
            }
        } catch (const std::exception&) {
            outcome.detail = "T3 communication failed; check the saved action before retrying";
        }
        applyDelivery(id,action,std::move(outcome));
    }

    std::vector<std::string> available(const Json& task, const Json& event,
                                       const std::optional<PeerExchange>& peer, bool canSend, bool canRead) {
        if (event["kind"] == "bot_question") return {"reply_to_bot","ask_user"};
        if (peer && peer->recipient == task["id"]) {
            if (event["kind"] == "question") return {"answer_session_question","ask_user"};
            if (peer->kind == "consult") return {"reply_to_bot"};
            if (event["kind"] == "result" || event.contains("sessionResult")) {
                std::vector<std::string> tools{"delegate","follow_up","reply_to_bot"};
                if (options.sessionRead == "on_demand" && event["evidence"].size() < 3) tools.push_back("session_read");
                return tools;
            }
            return {"delegate","reply_to_bot"};
        }
        if (event["kind"] == "question") return {"answer_session_question","ask_user"};
        std::vector<std::string> tools{"delegate","ask_user"};
        const bool hasReport = event["kind"] == "result" || event.contains("sessionResult");
        if (hasReport && !task["lastRun"].is_null()) {
            tools.push_back("follow_up");
            if (options.sessionRead == "on_demand" && event["evidence"].size() < 3) tools.push_back("session_read");
        }
        if (event["kind"] == "result" && !task["lastRun"].is_null() &&
            revision(task) == event.value("revisionId",Json(nullptr))) tools.push_back("complete");
        if (event["kind"] == "bot_result") {
            if ((event["requestKind"] == "consult" && event["status"] == "completed") ||
                !field(task,"doneWhen").empty()) tools.push_back("complete");
            if (event["evidence"].size() < 3 && canRead) tools.push_back("read_bot_evidence");
        }
        if (canSend) tools.push_back("send_bot_message");
        return tools;
    }
    void failure(const Json& task, const Json& event, std::string_view detail, bool transient) {
        change(task,[&](Json& current) {
            const auto attempts = event["failures"].get<int>() + 1;
            const auto why = bounded(db.redactor(field(task,"botId")).text(std::string(detail)),2048);
            db.exec("UPDATE events SET failures=?,retry_at=? WHERE id=?",
                    {attempts, transient ? options.clock() + (1 << (attempts - 1)) : 0,event["id"]});
            db.journal(current["id"],"model_failure",{{"eventId",event["id"]},{"attempt",attempts},{"text",why}});
            current["feedback"] = why;
            if (attempts >= 3) db.wait(current,"model_failure",{{"text",why}});
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
        const auto text = bounded(scrub.text(excerpt.text),8192);
        return {{"environmentId",req.environmentId},{"threadId",req.threadId},{"runId",req.runId},
                {"itemId",excerpt.itemId},{"text",text},{"more",excerpt.more || text.size() < excerpt.text.size()},
                {"itemTruncated",excerpt.nextOffset > req.offset || text.size() < excerpt.text.size()},
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
            req.environmentId = report.value("environmentId",req.environmentId);
        } else {
            std::lock_guard lock(db.mutex);
            auto row = db.query("SELECT DISTINCT environment_id FROM runs WHERE task_id=? AND thread_id=? AND run_id=? LIMIT 2",
                                {task["id"],req.threadId,req.runId});
            if (!row.next()) throw Conflict("History reference does not belong to this task");
            req.environmentId = row.string(0);
            if (row.next()) throw Conflict("History reference matches more than one Session");
        }
        if (req.threadId.empty() || req.runId.empty()) throw Conflict("History reference does not belong to this task");
        if (!access.permits(field(task,"botId"),Scope::sessionRead(req.environmentId,req.threadId),true)) throw Conflict("Session read access required");
        auto entry = readExcerpt(field(task,"botId"),req);
        return change(task,[&](Json& current) {
            auto evidence = event["evidence"];
            evidence.push_back(entry);
            db.exec("UPDATE events SET evidence=? WHERE id=?", {evidence,event["id"]});
            if (!usage.empty()) entry["usage"] = usage;
            db.journal(current["id"],"session_read",entry);
            return true;
        });
    }

    enum class PrefillResult { NotNeeded, Persisted, UnavailableWithSettledReport };

    PrefillResult prefillHistory(const Json& task, const Json& event) {
        if (options.sessionRead != "prefill" || !event["evidence"].empty() ||
            (event["kind"] != "result" && !event.contains("sessionResult")))
            return PrefillResult::NotNeeded;
        const auto& report = event.contains("sessionResult") ? event["sessionResult"] : event;
        const auto run = runRef(report["run"]);
        if (!access.permits(field(task,"botId"),Scope::sessionRead(report.value("environmentId",field(task,"environmentId")),run.threadId),true))
            return PrefillResult::NotNeeded;
        try {
            readEvidence(task,event,{{"threadId",run.threadId},{"runId",run.runId},{"view","activity"}});
            return PrefillResult::Persisted;
        } catch (const HistoryUnavailable&) {
            if (!event.contains("sessionResult")) throw;
            return PrefillResult::UnavailableWithSettledReport;
        }
    }

    void queueAction(Json& task, const std::string& action, std::string_view tool, std::string instruction,
                     const Json& args, const Json& pending = Json::array(), bool charge = false) {
        const bool work = tool == "delegate" || tool == "follow_up";
        if (charge) {
            task["remaining"] = task["remaining"].get<int>() - 1;
            task["additionalUsed"] = task["additionalUsed"].get<int>() + 1;
        }
        Json resultReferences = Json::array();
        if (work) {
            if (task.contains("sourceTaskId") && task["lastRun"].is_null()) {
                const auto request = field(task,"request");
                if (instruction.find(request) == std::string::npos) instruction += "\n\nUser request:\n" + request;
            }
            includeInstructions(instruction,pending);
            if (const auto restart = task.find("restartContext"); restart != task.end() && task["lastRun"].is_null()) {
                instruction += "\n\nOriginal task being resumed:\n" + field(*restart,"intent") +
                    "\n\nCompletion criteria:\n" + field(task,"doneWhen");
                includeInstructions(instruction,restart->value("pendingInstructions",Json::array()));
            }
            std::size_t excerptBudget = 8192;
            std::set<std::pair<Id,int>> included;
            const auto quoteSource = [&](const Json& source) {
                const auto id = source.value("messageId",Id{0});
                if (!id || !included.emplace(id,source.value("option",0)).second) return;
                auto reference = source;
                reference.erase("text");
                resultReferences.push_back(std::move(reference));
                instruction += "\n\nReferenced result " + std::to_string(id) + " from task " +
                    std::to_string(source.at("taskId").get<Id>()) +
                    (source.value("option",0) ? ", option " + std::to_string(source.at("option").get<int>()) : "") +
                    " (reference only):";
                const auto excerpt = bounded(field(source,"text"),excerptBudget);
                if (!excerpt.empty()) instruction += "\n" + excerpt;
                excerptBudget -= excerpt.size();
            };
            for (const auto& item : pending)
                if (item.contains("source")) quoteSource(item.at("source"));
            if (task["lastRun"].is_null()) {
                for (const auto& source : task.value("sourceResultRefs",Json::array()))
                    if (source["taskId"] == task.value("sourceTaskId",Id{0})) quoteSource(source);
            }
            instruction += "\n\n[Flock action ID: " + action + "]";
            task["deadline"] = 0;
        }
        const Json out = {{"id",action},{"tool",tool},{"environmentId",task["environmentId"]},
            {"target",tool == "follow_up" ? task["lastRun"] : task["activeRun"]},
            {"instruction",std::move(instruction)},{"questionId",field(args,"requestId")},
            {"answers",args.value("answers",Json::object())},{"revisionId",revision(task)},
            {"instructions",pending},{"sourceResultRefs",resultReferences},{"chargesBudget",charge}};
        db.exec("INSERT INTO outbox(id,task_id,phase,data) VALUES(?,?,'ready',?)", {action,task["id"],out});
        task["actionId"] = action;
        task["state"] = "sending";
    }

    bool needsBudget(Json& task, std::string_view tool, const std::string& reason, const Json& usage) {
        if (task["remaining"] != 0) return false;
        db.wait(task,"budget",{{"text","The additional work allowance is exhausted"},{"attempted",tool},
                              {"decisionReason",reason},{"usage",usage}});
        return true;
    }

    std::string priorInstructions(const Json& task) {
        auto rows = db.query(R"SQL(
SELECT entry.value FROM outbox, json_each(outbox.data,'$.instructions') entry
WHERE outbox.task_id=? AND outbox.phase='accepted'
ORDER BY outbox.rowid DESC, entry.key DESC LIMIT 20
)SQL", {task["id"]});
        std::vector<std::string> excerpts;
        std::size_t remaining = 4000;
        bool sawTarget = false;
        while (remaining && rows.next()) {
            const auto item = rows.json(0);
            const auto key = replacementKey(item);
            if (key == "target" && std::exchange(sawTarget,true)) continue;
            if (!key.empty() && key != "target") continue;
            auto text = bounded(field(item,"text"),remaining);
            remaining -= text.size();
            excerpts.push_back(std::move(text));
        }
        if (excerpts.empty()) return {};
        std::string context = "\n\nEarlier user directions from this Task's prior Session (excerpts):";
        for (auto item = excerpts.rbegin(); item != excerpts.rend(); ++item) context += "\n\n" + *item;
        return context;
    }

    void continueTask(const Json& task) {
        change(task,[&](Json& current) {
            const auto pending = pendingBatch(current);
            if (pending.instructions.empty()) return false;
            const bool fresh = current.value("newSessionRequired",false);
            const std::string tool = fresh ? "delegate" : "follow_up";
            const std::string reason = "Apply the accepted user instructions at a settled Run boundary";
            if (needsBudget(current,tool,reason,Json::object())) return true;
            const auto action = actionId();
            auto instruction = "Continue this Flock task.\n\nTask intent:\n" + intent(current) +
                "\n\nCompletion criteria:\n" + field(current,"doneWhen");
            if (fresh) instruction += priorInstructions(current);
            db.consumeEvent(current);
            db.journal(current["id"],"decision",{{"actionId",action},{"tool",tool},{"reason",reason},
                {"eventId",current["eventId"]},{"revisionId",revision(current)}});
            supersedeInstructions(current,pending.superseded);
            queueAction(current,action,tool,std::move(instruction),Json::object(),pending.instructions,true);
            return true;
        });
    }

    std::vector<PeerBot> peersFor(const Json& task) const {
        if (!options.peers) return {};
        return options.peers(field(task,"botId"));
    }

    PeerBot allowedPeer(const Json& task, std::string_view botId) const {
        if (botId.empty() || task["botId"] == botId) throw Conflict("Choose an allowed peer Bot");
        const auto peers = peersFor(task);
        const auto peer = std::find_if(peers.begin(),peers.end(),[&](const PeerBot& p) { return p.intake.botId == botId; });
        if (peer == peers.end()) throw Conflict("The destination Bot is unavailable or not allowed");
        return *peer;
    }

    void ensureNoRouteCycle(const Json& sender, const Intake& target) const {
        if (sender["environmentId"] == target.environmentId && sender["projectId"] == target.projectId) return;
        auto cycle = db.query(R"SQL(
WITH RECURSIVE dependencies(source_env,source_project,target_env,target_project) AS (
  SELECT s.environment_id,s.project_id,t.environment_id,t.project_id
  FROM peer_exchanges p JOIN tasks s ON s.id=p.sender_task JOIN tasks t ON t.id=p.recipient_task
  WHERE p.kind='work' AND p.phase<>'closed'
), reachable(env,project) AS (
  VALUES(?,?)
  UNION
  SELECT target_env,target_project FROM dependencies JOIN reachable
  ON source_env=env AND source_project=project
)
SELECT 1 FROM reachable WHERE env=? AND project=?
)SQL",{target.environmentId,target.projectId,sender["environmentId"],sender["projectId"]});
        if (cycle.next()) throw Conflict("This work request would make two project queues wait for one another");
    }

    Json peerMessage(const PeerExchange& peer, const Json& from, const Json& to, std::string_view type,
                     std::string_view body, std::string_view messageId) {
        Json message = {{"exchangeId",peer.id},{"messageId",messageId},{"fromBotId",from["botId"]},
            {"toBotId",to["botId"]},{"messageType",type},{"requestKind",peer.kind},{"text",body}};
        if (!peer.lastMessage.empty()) message["replyTo"] = peer.lastMessage;
        return message;
    }

    void peerEvent(Json& task, Json message, std::string_view kind) {
        db.consumeEvent(task);
        task.erase("feedback");
        if (message.contains("evidence")) {
            message["references"] = std::move(message["evidence"]);
            message.erase("evidence");
        }
        message["kind"] = kind;
        const auto key = "bot/" + field(message,"messageId");
        db.event(task,key,std::move(message));
        if (task["state"] != "queued") task["state"] = "deciding";
    }

    void recordDecision(const Json& task, const Json& event, const Decision& decision, const Json& usage, std::string_view action) {
        db.journal(task["id"],"decision",{{"actionId",action},{"tool",decision.tool},{"args",decision.args},
            {"reason",decision.reason},{"eventId",event["id"]},{"evidence",event["evidence"]},{"usage",usage},
            {"revisionId",revision(task)}});
    }

    Json forwardableUserMessage(const Json& task) const {
        const auto sourceId = task.value("sourceMessageId",Id{0});
        if (!sourceId) return nullptr;
        // A compound or clarified request may not represent one complete user message.
        auto row = db.query(R"SQL(
SELECT m.text,j.time FROM messages m JOIN journal j ON j.id=m.id
WHERE m.id=? AND m.bot_id=? AND m.text=? AND j.kind='conversation_message'
)SQL",{sourceId,task.at("botId"),task.at("request")});
        if (!row.next()) return nullptr;
        return {{"text",row.string(0)},{"time",row.number(1)}};
    }

    void sendPeer(Json& sender, const Json& event, const Decision& decision, const Json& usage, PeerBot peer, const std::string& action) {
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
        const bool initial = field(sender,"doneWhen").empty();
        if (work) {
            if (!sender["activeRun"].is_null() || !field(sender,"actionId").empty())
                throw Conflict("Let the assigned Run settle before requesting another Bot's work");
            if (initial) {
                const auto source = sender.value("criteriaSourceMessageId",sender.value("sourceMessageId",Id{0}));
                db.addRevision(sender,intent(sender),sender.value("requestedDoneWhen",field(decision.args,"doneWhen")),source);
                sender.erase("requestedDoneWhen");
                sender.erase("criteriaSourceMessageId");
            }
            else if (sender["remaining"] == 0) {
                db.wait(sender,"budget",{{"text","The additional work allowance is exhausted"},
                    {"attempted",decision.tool},{"decisionReason",decision.reason},{"usage",usage}});
                return;
            } else {
                sender["remaining"] = sender["remaining"].get<int>() - 1;
                sender["additionalUsed"] = sender["additionalUsed"].get<int>() + 1;
            }
            ensureNoRouteCycle(sender,peer.intake);
        }
        peer.intake.request = note.empty() ? field(forwarded,"text") : note;
        peer.intake.scheduleId.clear();
        peer.intake.scheduledAt = 0;
        const auto id = db.submit(peer.intake,0,nullptr,true);
        auto recipient = db.load(id);
        const PeerExchange exchange{action,kind,"working","",sender["id"],id};
        db.exec("INSERT INTO peer_exchanges(id,kind,phase,last_message,sender_task,recipient_task)"
                " VALUES(?,?,'working',?,?,?)",{action,kind,action,sender["id"],id});
        auto message = peerMessage(exchange,sender,recipient,"request",note,action);
        if (!forwarded.is_null()) message["forwardedMessage"] = std::move(forwarded);
        db.peerMessage(exchange,message);
        peerEvent(recipient,std::move(message),"bot_request");
        db.save(recipient);
        db.consumeEvent(sender);
        sender.erase("feedback");
        recordDecision(sender,event,decision,usage,action);
        sender["state"] = "peer_waiting";
    }

    Json settledSessionReport(const Json& task) const {
        const auto run = runRef(task["lastRun"]);
        if (!hasRun(run)) return nullptr;
        auto report = db.query("SELECT data FROM journal WHERE task_id=? AND kind='report' ORDER BY id DESC LIMIT 1",
                               {task["id"]});
        if (!report.next()) return nullptr;
        auto data = report.json(0);
        const auto reported = runRef(data["run"]);
        if (reported.threadId != run.threadId || reported.runId != run.runId) return nullptr;
        return data;
    }

    Json resultReference(const Json& task) const {
        const auto report = settledSessionReport(task);
        if (report.is_null()) return nullptr;
        const auto run = runRef(report["run"]);
        return {{"environmentId",report.value("environmentId",task["environmentId"])},{"threadId",run.threadId},{"runId",field(report,"resultRunId")},
                {"itemId",field(report,"itemId")},{"link",run.link},{"status",field(report,"status")}};
    }

    void finishPeer(const PeerExchange& exchange, const Json& recipient, std::string_view status, std::string_view text,
                    const std::string& action) {
        auto sender = db.load(exchange.sender);
        const auto type = status == "completed" ? std::string_view{"result"} : status;
        auto message = peerMessage(exchange,recipient,sender,type,text,action);
        message["status"] = status;
        if (status != "cancelled") {
            const auto ref = resultReference(recipient);
            if (!ref.is_null()) message["evidence"] = Json::array({ref});
        }
        db.exec("UPDATE peer_exchanges SET phase='closed',last_message=? WHERE id=?",{action,exchange.id});
        db.peerMessage(exchange,message);
        if (!terminal(sender) && sender["state"] != "cancelling") {
            db.clearWait(sender,"bot_result");
            peerEvent(sender,std::move(message),"bot_result");
            db.save(sender);
        }
    }

    void replyPeer(Json& task, const Json& event, const Decision& decision, const Json& usage, const std::string& action) {
        const auto peer = db.peerExchange(task["id"]);
        if (!peer) throw Conflict("The Bot exchange has closed");
        const auto type = field(decision.args,"kind");
        const auto body = field(decision.args,"text");
        const bool answering = type == "answer" && peer->sender == task["id"] && peer->phase == "question";
        const bool receiving = peer->recipient == task["id"] && peer->phase == "working";
        if (!answering && !(receiving && (type == "question" || type == "result" || type == "failed")))
            throw Conflict("This reply does not match the current Bot exchange");
        if (type == "result" && peer->kind == "work" &&
            (hasPending(task) || task.value("replanning",false) ||
             (event["kind"] == "result" && revision(task) != event.value("revisionId",Json(nullptr)))))
            throw Conflict("Apply the pending revision and complete revised work before returning a result");
        const auto otherId = answering ? peer->recipient : peer->sender;
        auto other = db.load(otherId);
        if (terminal(other) || other["state"] != "peer_waiting") throw Conflict("The other Bot is no longer waiting");
        db.consumeEvent(task);
        task.erase("feedback");
        recordDecision(task,event,decision,usage,action);
        if (type == "result" || type == "failed") {
            const auto status = type == "result" ? "completed" : "failed";
            task["state"] = "completed";
            task["peerOutcome"] = status;
            finishPeer(*peer,task,status,body,action);
            db.journal(task["id"],"peer_completed",{{"text",body},{"status",status},
                {"reason",decision.reason},{"doneWhen",task["doneWhen"]}});
        } else {
            db.exec("UPDATE peer_exchanges SET phase=?,last_message=? WHERE id=?",
                    {answering ? "working" : "question",action,peer->id});
            auto message = peerMessage(*peer,task,other,type,body,action);
            db.peerMessage(*peer,message);
            peerEvent(other,std::move(message),answering ? "bot_answer" : "bot_question");
            db.save(other);
            task["state"] = "peer_waiting";
        }
    }

    void readPeerEvidence(const Json& task, const Json& event, const Decision& decision, const Json& usage) {
        const auto& args = decision.args;
        const auto messageId = field(args,"messageId");
        if (event["kind"] != "bot_result" || messageId.empty() || event["messageId"] != messageId)
            throw Conflict("Choose the Bot result for the current exchange");
        allowedPeer(task,field(event,"fromBotId"));
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
        if (!access.permits(field(task,"botId"),Scope::sessionRead(req.environmentId,req.threadId),true)) throw Conflict("Session read access required");
        auto entry = readExcerpt(field(task,"botId"),req);
        entry["messageId"] = messageId;
        entry["exchangeId"] = event["exchangeId"];
        change(task,[&](Json& current) {
            auto evidence = event["evidence"];
            evidence.push_back(entry);
            db.exec("UPDATE events SET evidence=? WHERE id=?",{evidence,event["id"]});
            if (!usage.empty()) entry["usage"] = usage;
            db.journal(current["id"],"read_bot_evidence",entry);
            return true;
        });
    }

    // An unfinished conversation turn can still change any task of its Bot. Call alongside the closing write.
    bool deferUntilTurnsFinish(const Json& current, Id eventId, const Decision& decision, const Json& usage) {
        if (!db.conversationPending(field(current,"botId"))) return false;
        auto row = db.query("SELECT data FROM events WHERE id=? AND task_id=? AND consumed=0",{eventId,current["id"]});
        if (!row.next()) return true;
        auto data = row.json(0);
        data["_flockDeferredCompletion"] = true;
        // Preserve the task generation and reuse the decision if the turn leaves the task unchanged.
        data["_flockSavedCompletion"] = {{"generation",current["generation"]},{"tool",decision.tool},
            {"args",decision.args},{"reason",decision.reason},{"usage",usage}};
        db.exec("UPDATE events SET data=? WHERE id=? AND task_id=? AND consumed=0",{data,eventId,current["id"]});
        return true;
    }

    void acceptCompletion(const Json& snapshot, const Json& event, const Decision& decision, const Json& usage) {
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        const auto id = snapshot["id"].get<Id>();
        const auto eventId = event["id"].get<Id>();
        auto current = db.load(id);
        if (current["generation"] != snapshot["generation"] || current.value("eventId",Id{0}) != eventId ||
            hasPending(current) || current.value("replanning",false) ||
            (event["kind"] == "result" && revision(current) != event.value("revisionId",Json(nullptr)))) return;

        if (deferUntilTurnsFinish(current,eventId,decision,usage)) {
        } else {
            clearDeferredCompletionMarker(id,eventId);
            db.consumeEvent(current);
            recordDecision(current,event,decision,usage,actionId());
            current.erase("feedback");
            current["state"] = "completed";
            db.journal(id,"completed",{{"text",field(decision.args,"report")},{"reason",decision.reason},
                {"doneWhen",current["doneWhen"]},{"revisionId",revision(current)},
                {"source",{{"environmentId",field(event,"environmentId")},
                    {"threadId",field(event.value("run",Json(nullptr)),"threadId")},
                    {"runId",field(event,"resultRunId")},{"itemId",field(event,"itemId")},
                    {"link",field(event.value("run",Json(nullptr)),"link")}}}});
            db.save(current);
        }
        transaction.commit();
    }

    void accept(const Json& snapshot, const Json& event, const Decision& decision, const Json& usage) {
        const auto& args = decision.args;
        const auto& tool = decision.tool;
        static const std::map<std::string_view,std::set<std::string_view>> arguments{
            {"delegate",{"instruction","doneWhen"}},
            {"answer_session_question",{"requestId","answers"}},
            {"follow_up",{"instruction"}},
            {"complete",{"report"}},
            {"ask_user",{"question","choices"}},
            {"session_read",{"threadId","runId","itemId","cursor","view","offset"}},
            {"send_bot_message",{"botId","kind","text","doneWhen","forwardUserMessage"}},
            {"reply_to_bot",{"kind","text"}},
            {"read_bot_evidence",{"messageId","itemId","cursor","offset"}}
        };
        for (auto item = args.begin(); item != args.end(); ++item) {
            if (!arguments.at(tool).contains(item.key())) throw Conflict("Unknown action argument");
        }
        if (tool == "session_read") {
            readEvidence(snapshot,event,args,usage);
            return;
        }
        if (tool == "read_bot_evidence") return readPeerEvidence(snapshot,event,decision,usage);
        if (tool == "send_bot_message" || tool == "reply_to_bot") {
            if (args.contains("forwardUserMessage") && !args.at("forwardUserMessage").is_boolean())
                throw Conflict("forwardUserMessage must be a boolean");
            const auto text = field(args,"text");
            if ((args.contains("text") && !args.at("text").is_string()) ||
                (!present(text) && !(text.empty() && args.value("forwardUserMessage",false))))
                throw Conflict("A Bot message requires text or a forwarded user message");
            if (tool == "reply_to_bot") {
                // Check unfinished turns in the same transaction that closes the exchange.
                const auto kind = field(args,"kind");
                std::lock_guard lock(db.mutex);
                Transaction transaction(db.db);
                auto current = db.load(snapshot["id"]);
                if (current["generation"] != snapshot["generation"]) return;
                if ((kind == "result" || kind == "failed") &&
                    deferUntilTurnsFinish(current,event["id"].get<Id>(),decision,usage)) {
                    transaction.commit();
                    return;
                }
                replyPeer(current,event,decision,usage,actionId());
                db.save(current);
                transaction.commit();
                return;
            }
            const auto kind = field(args,"kind");
            if (kind != "consult" && kind != "work") throw Conflict("Choose a consultation or work request");
            const bool requiresCondition = kind == "work" && field(snapshot,"doneWhen").empty();
            if ((requiresCondition && !present(field(args,"doneWhen"))) || (!requiresCondition && args.contains("doneWhen")))
                throw Conflict("Only the first work request sets the task's acceptance criteria");
            auto peer = allowedPeer(snapshot,field(args,"botId"));
            change(snapshot,[&](Json& current) { sendPeer(current,event,decision,usage,std::move(peer),actionId()); return true; });
            return;
        }
        const bool work = tool == "delegate" || tool == "follow_up";
        const bool initial = field(snapshot,"doneWhen").empty();
        const auto instruction = field(args,"instruction");
        const auto requested = field(snapshot,"requestedDoneWhen");
        const auto doneWhen = requested.empty() ? field(args,"doneWhen") : requested;
        if (work && !present(instruction)) throw Conflict("A delegate/follow-up instruction is required");
        if (tool == "delegate" && initial && !present(doneWhen)) throw Conflict("Initial delegate requires doneWhen");
        if ((!initial || tool != "delegate") && args.contains("doneWhen")) throw Conflict("doneWhen is already fixed or unavailable");
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
            return acceptCompletion(snapshot,event,decision,usage);
        }
        if (tool == "answer_session_question") {
            if (field(args,"requestId") != field(snapshot["question"],"requestId"))
                throw Conflict("The Session question no longer matches");
            validateAnswers(snapshot["question"]["fields"],args.value("answers",Json::object()));
        }
        change(snapshot,[&](Json& current) {
            const auto id = current["id"].get<Id>();
            if (tool == "follow_up" && current.value("newSessionRequired",false))
                throw Conflict("Start a new Session in the current environment");
            const bool charge = work && (!current["lastRun"].is_null() || db.query(
                "SELECT 1 FROM peer_exchanges WHERE sender_task=? AND kind='work' LIMIT 1",{id}).next());
            if (charge && needsBudget(current,tool,decision.reason,usage)) return true;
            if (initial && work) {
                const auto source = current.value("criteriaSourceMessageId",current.value("sourceMessageId",Id{0}));
                db.addRevision(current,intent(current),doneWhen,source);
                current.erase("requestedDoneWhen");
                current.erase("criteriaSourceMessageId");
            }
            clearDeferredCompletionMarker(id,event["id"].get<Id>());
            db.consumeEvent(current);
            const auto action = actionId();
            recordDecision(current,event,decision,usage,action);
            current.erase("feedback");
            if (tool == "ask_user") {
                db.wait(current,"user_question",{{"text",field(args,"question")},{"decisionReason",decision.reason},
                    {"questionId",field(current["question"],"requestId")},{"choices",args.value("choices",Json::array())}});
            } else {
                const auto pending = work ? pendingBatch(current) : InstructionBatch{};
                supersedeInstructions(current,pending.superseded);
                auto workInstruction = work ? instruction : "";
                const auto userAnswer = field(event,"userAnswer");
                if (work && !userAnswer.empty() && workInstruction.find(userAnswer) == std::string::npos)
                    workInstruction += "\n\nUser answer:\n" + userAnswer;
                if (tool == "delegate" && current.value("newSessionRequired",false) && event["kind"] == "bot_answer") {
                    workInstruction += "\n\nTask intent:\n" + intent(current) +
                        "\n\nCompletion criteria:\n" + field(current,"doneWhen") + priorInstructions(current);
                    workInstruction += "\n\nAnswer from the requesting Bot:\n" + field(event,"text");
                    const auto report = field(event.value("sessionResult",Json(nullptr)),"report");
                    if (!report.empty()) workInstruction += "\n\nPrevious Session report (reference only):\n" + report;
                }
                queueAction(current,action,tool,std::move(workInstruction),args,pending.instructions,charge);
            }
            return true;
        });
    }

    void decide(const Json& task) {
        if (!task["activeRun"].is_null()) {
            const auto observed = observe(task);
            if (ingest(task,observed) || observed.kind != Observation::Kind::Question) return;
        }
        Json event, history = Json::array();
        std::optional<PeerExchange> peer;
        std::optional<Decision> savedCompletion;
        Json savedUsage;
        {
            std::lock_guard lock(db.mutex);
            event = db.currentEvent(task);
            if (event.is_null() || event["consumed"] == true || event["retryAt"].get<Seconds>() > options.clock()) return;
            event.erase("_flockDeferredCompletion");
            const auto deferred = event.value("_flockSavedCompletion",Json(nullptr));
            event.erase("_flockSavedCompletion");
            if (deferred.is_object() && deferred["generation"] == task["generation"]) {
                if (db.conversationPending(field(task,"botId"))) return;
                savedCompletion = Decision{deferred.value("tool","complete"),deferred["args"],deferred["reason"]};
                savedUsage = deferred["usage"];
            } else {
                peer = db.peerExchange(task["id"]);
                if (event["kind"] == "bot_answer" || event["kind"] == "bot_result") {
                    auto report = settledSessionReport(task);
                    if (!report.is_null()) event["sessionResult"] = std::move(report);
                }
                auto rows = db.query(R"SQL(
SELECT id,kind,data,json_extract(data,'$.run.threadId'),json_extract(data,'$.resultRunId'),
       json_extract(data,'$.itemId') FROM journal WHERE task_id=?
AND kind IN ('decision','report','user_answer','session_answered','bot_message') ORDER BY id DESC LIMIT 6
)SQL",{task["id"]});
                std::size_t remaining = 16000;
                while (rows.next() && remaining > 100) {
                    const auto data = rows.string(2);
                    const auto excerpt = bounded(data,std::min<std::size_t>(remaining,5000));
                    if (data.size() == excerpt.size()) history.push_back({{"id",rows.number(0)},{"kind",rows.string(1)},{"data",Json::parse(data)}});
                    else history.push_back({{"id",rows.number(0)},{"kind",rows.string(1)},{"excerpt",excerpt},{"truncated",true},
                        {"source",{{"environmentId",task["environmentId"]},{"threadId",rows.string(3)},
                            {"runId",rows.string(4)},{"itemId",rows.string(5)}}}});
                    remaining -= excerpt.size();
                }
                std::reverse(history.begin(),history.end());
            }
        }
        try {
            if (savedCompletion) {
                if (savedCompletion->tool == "complete") return acceptCompletion(task,event,*savedCompletion,savedUsage);
                return accept(task,event,*savedCompletion,savedUsage);
            }
            auto choices = Json::array();
            bool canRead = false;
            if (!peer && task["activeRun"].is_null()) {
                for (const auto& allowed : peersFor(task)) {
                    choices.push_back({{"botId",allowed.intake.botId},{"name",allowed.name}});
                    if (event.contains("references") && !event["references"].empty() && event["fromBotId"] == allowed.intake.botId)
                        canRead = true;
                }
            }
            auto tools = available(task,event,peer,!choices.empty(),canRead);
            if (task.value("newSessionRequired",false)) std::erase(tools,"follow_up");
            if (hasPending(task)) std::erase(tools,"complete");
            if (prefillHistory(task,event) == PrefillResult::Persisted) return;
            const auto bot = notes(field(task,"botId"));
            Json summary = {{"id",task["id"]},{"botId",task["botId"]},{"request",intent(task)},
                {"doneWhen",task["doneWhen"]},
                {"remaining",task["remaining"]},{"additionalUsed",task["additionalUsed"]},{"revisionId",revision(task)}};
            if (task.contains("requestedDoneWhen")) summary["requestedDoneWhen"] = task.at("requestedDoneWhen");
            if (task.contains("restartContext")) summary["restartContext"] = task.at("restartContext");
            if (hasPending(task)) {
                const auto pending = pendingBatch(task);
                Json applicable = Json::array();
                std::size_t bytes = 2, omitted = 0;
                for (const auto& item : pending.instructions) {
                    const auto size = item.dump().size() + 1;
                    if (!applicable.empty() && bytes + size > 32000) { ++omitted; continue; }
                    applicable.push_back(item);
                    bytes += size;
                }
                if (omitted) applicable.push_back({{"omittedInstructions",omitted}});
                summary["userInstructions"] = applicable.dump();
            }
            if (task.contains("sourceResultRefs")) summary["sourceResultRefs"] = task["sourceResultRefs"];
            if (!choices.empty()) summary["peers"] = std::move(choices);
            if (std::ranges::find(tools,"send_bot_message") != tools.end()) {
                std::lock_guard lock(db.mutex);
                auto forwarded = forwardableUserMessage(task);
                if (!forwarded.is_null()) summary["forwardableUserMessage"] = std::move(forwarded);
            }
            if (peer) summary["peer"] = {{"exchangeId",peer->id},{"requestKind",peer->kind},
                {"role",peer->sender == task["id"] ? "sender" : "recipient"}};
            const auto evidence = event["evidence"];
            event.erase("evidence");
            event.erase("consumed");
            Id attempt = 0;
            {
                std::lock_guard lock(db.mutex);
                Transaction transaction(db.db);
                auto current = db.load(task["id"]);
                if (current["generation"] != task["generation"]) return;
                const auto budget = db.taskModelBudget(current);
                attempt = db.reserveModelCall(budget,"task_decision",0,task["id"],event["id"]);
                if (!attempt) {
                    db.wait(current,"model_budget",{{"text",
                        "The model call limit is exhausted. Approve one more model call to continue."}});
                    db.save(current);
                }
                transaction.commit();
            }
            if (!attempt) return;
            const auto turn = db.callModel(field(task,"botId"),model,
                {summary,event,history,bot,tools,field(task,"feedback"),evidence,task["config"]},attempt);
            if (turn.calls.size() != 1) throw Conflict("Return one task action");
            const auto& decision = turn.calls.front();
            event["evidence"] = evidence;
            if (!task["activeRun"].is_null()) {
                const auto observed = observe(task);
                if (ingest(task,observed) || observed.kind != Observation::Kind::Question) return;
            }
            if (!decision.args.is_object() || decision.args.dump().size() > 20000 || !present(decision.reason,4000) ||
                std::find(tools.begin(),tools.end(),decision.tool) == tools.end())
                throw Conflict("Return exactly one permitted action, valid arguments and a reason");
            accept(task,event,decision,turn.usage);
        } catch (const ModelError& e) {
            failure(task,event,e.what(),e.transient);
        } catch (const std::exception& e) {
            failure(task,event,e.what(),false);
        }
    }

    bool peerExchangeDefersReplan(Id id) {
        std::lock_guard lock(db.mutex);
        const auto peer = db.peerExchange(id);
        return peer && !(peer->recipient == id && peer->kind == "work" && peer->phase == "working");
    }

    void step(Id id) {
        const auto task = snapshot(id);
        const auto state = field(task,"state");
        if (state == "cancelling") cancel(task);
        else if (task.value("replanning",false) && !peerExchangeDefersReplan(id)) replan(task);
        else if (state == "queued") activate(task);
        else if (state == "deciding") {
            bool canContinue = false;
            if (task["activeRun"].is_null() && !task["lastRun"].is_null() && hasPending(task)) {
                std::lock_guard lock(db.mutex);
                const auto event = db.currentEvent(task);
                canContinue = (event.is_null() || event["consumed"] == true || event["kind"] == "result") &&
                              field(event,"userAnswer").empty();
            }
            if (canContinue) continueTask(task);
            else decide(task);
        }
        else if (state == "sending") {
            Json action;
            {
                std::lock_guard lock(db.mutex);
                action = db.outbox(task);
            }
            if (!action.is_null()) deliver(task,action);
        } else if (!task["activeRun"].is_null() && (state == "running" || state == "waiting"))
            ingest(task,observe(task));
    }
};

Runtime::Runtime(Store& store, Model& model, T3& t3, NotesLoader notes, RuntimeOptions options)
    : p_(std::make_unique<Impl>(store,model,t3,std::move(notes),std::move(options))) {}
Runtime::~Runtime() = default;
void Runtime::runConversations() {
    constexpr auto messagesPerTick = 20;
    for (int i = 0; i < messagesPerTick; ++i) {
        const auto pending = p_->db.pendingConversations(1);
        if (pending.empty() || !p_->db.runTurn(pending.front().second,p_->model,p_->t3,p_->notes,
            *p_->options.botTools)) break;
    }
}
void Runtime::tick() {
    runConversations();
    for (auto id : p_->eligible()) p_->step(id);
}
void Runtime::tickConcurrent() {
    const auto collect = [](auto& workers) {
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto result = std::move(it->second);
                it = workers.erase(it);
                result.get();
            } else ++it;
        }
    };
    collect(p_->conversationWorkers);
    collect(p_->workers);
    constexpr auto maxConversationWorkers = 4u;
    if (p_->conversationWorkers.size() < maxConversationWorkers) {
        for (const auto& [bot,id] : p_->db.pendingConversations(maxConversationWorkers)) {
            if (p_->conversationWorkers.size() >= maxConversationWorkers) break;
            if (!p_->conversationWorkers.contains(bot)) {
                p_->conversationWorkers.emplace(bot,std::async(std::launch::async,[this,id] {
                    p_->db.runTurn(id,p_->model,p_->t3,p_->notes,
                        *p_->options.botTools);
                }));
            }
        }
    }
    for (auto id : p_->eligible()) {
        if (!p_->workers.contains(id))
            p_->workers.emplace(id,std::async(std::launch::async,[this,id] { p_->step(id); }));
    }
}
} // namespace flockv0
