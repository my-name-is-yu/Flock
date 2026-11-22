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

Json revision(const Json& task) { return task.value("currentRevision",Json(nullptr)); }

bool hasPending(const Json& task) {
    const auto pending = task.find("pendingInstructions");
    return pending != task.end() && !pending->empty();
}

struct SourceQuotes {
    std::size_t remaining = 8192;
    std::set<Id> included;

    std::string quote(const Json& source) {
        const auto id = source.at("messageId").get<Id>();
        if (!id || !included.insert(id).second) return {};
        auto text = "\n\nReferenced result " + std::to_string(id) + " from task " +
            std::to_string(source.at("taskId").get<Id>()) + " (reference only):";
        const auto excerpt = bounded(field(source,"text"),remaining);
        remaining -= excerpt.size();
        if (!excerpt.empty()) text += "\n" + excerpt;
        return text;
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
        const auto lockPath = std::filesystem::canonical(db.path).string() + ".runtime.lock";
        lockFd = open(lockPath.c_str(), O_CLOEXEC | O_CREAT | O_RDWR, 0600);
        if (lockFd < 0 || ::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
            if (lockFd >= 0) close(lockFd);
            throw Conflict("Cannot acquire the exclusive Runtime lock for this database");
        }
    }
    ~Impl() {
        conversationWorkers.clear();
        workers.clear();
        close(lockFd);
    }

    void saveTask(Json& task) {
        task.erase("pendingDecision");
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
    static constexpr unsigned batchLimit = 4;
    Id lastScheduled = 0;

    std::vector<Id> nextProjects(std::size_t slots) {
        auto ids = eligible();
        std::erase_if(ids,[&](Id id) { return workers.contains(id); });
        if (ids.size() > slots) {
            std::rotate(ids.begin(),std::upper_bound(ids.begin(),ids.end(),lastScheduled),ids.end());
            ids.resize(slots);
        }
        if (!ids.empty()) lastScheduled = ids.back();
        return ids;
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
        task.erase("pendingEnvironmentId");
        task.erase("environmentReleased");
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

    bool ingest(const Json& snapshot, const Observation& observed) {
        return change(snapshot,[&](Json& task) {
            const auto run = runRef(task["activeRun"]);
            const auto state = field(task,"state");
            const auto reason = field(task["wait"],"reason");
            const bool replanning = task.value("replanning",false);
            const bool settledRunReleasesStopBarrier =
                observed.kind == Observation::Kind::Finished && observed.settled;
            if (settledRunReleasesStopBarrier) {
                closeQuestion(task,"run_finished");
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
                consumeEvent(task);
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
                        db.event(task,"result/" + field(task,"environmentId") + "/" + run.threadId + "/" + run.runId,data);
                    }
                    db.journal(task["id"],"report",data);
                }
                if (task.contains("runStop")) {
                    db.journal(task["id"],"interrupt_settled",task.at("runStop"));
                    task.erase("runStop");
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
                closeQuestion(task,"session_question_resolved","resolved_in_t3");
                consumeEvent(task);
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

    void interruptRun(const Json& task) {
        const auto& stop = task.at("runStop");
        if (task["activeRun"].is_null() || task["activeRun"] != stop.at("run") ||
            (task["state"] == "cancelling") != (stop["intent"] == "cancel")) return;
        const auto observed = observe(task);
        if (ingest(task,observed)) return;
        if (observed.kind == Observation::Kind::Finished)
            return stopNote(task,"Waiting for the assigned Run's delegated work to settle");
        if (observed.kind == Observation::Kind::Unavailable)
            return stopNote(task,"Cannot locate or observe the assigned Run");
        const auto now = options.clock();
        if (now < stop.value("retryAt",Seconds{0})) return;
        Json requested;
        if (!change(task,[&](Json& current) {
            current["runStop"]["retryAt"] = now + 5;
            db.journal(current["id"],"interrupt_requested",stop);
            return true;
        },&requested)) return;
        try { t3.interrupt(field(task,"environmentId"),task["config"],runRef(stop.at("run"))); }
        catch (const std::exception&) { stopNote(requested,"T3 did not confirm the interrupt request"); }
    }

    void finishReplan(const Json& task) {
        change(task,[&](Json& current) {
            const auto old = field(current,"environmentId");
            auto target = field(current,"pendingEnvironmentId");
            if (target.empty()) target = old;
            const auto id = current["id"].get<Id>();
            const auto peer = db.peerExchange(id);
            if (peer && !peer->allowsReplan(id)) return false;
            const auto event = db.currentEvent(current);
            const bool preservePeerAnswer = peer && !event.is_null() && event["consumed"] != true &&
                event["kind"] == "bot_answer";
            if (target != old && !current.value("environmentReleased",false)) {
                current["environmentReleased"] = true;
                return true;
            }
            if (target != old || current.value("environmentReleased",false) || current["state"] == "queued") {
                const auto blocker = db.queueBlocker(id);
                if (!blocker.is_null()) {
                    const bool waitingSender = peer && blocker["id"] == peer->sender &&
                        blocker["state"] == "peer_waiting";
                    if (!waitingSender) return false;
                }
            }
            if (target != old) {
                current["environmentId"] = target;
                current["newSessionRequired"] = true;
                db.journal(current["id"],"session_destination_changed",{{"from",old},{"to",target},{"revisionId",revision(current)}});
            }
            closeQuestion(current,"user_instruction");
            if (!preservePeerAnswer) {
                consumeEvent(current);
                auto report = settledSessionReport(current);
                db.event(current,"replan/" + std::to_string(current["generation"].get<Id>()),
                         report.is_null() ? Json{{"kind","intake"}} : report);
            }
            current["state"] = "deciding";
            for (const auto* key : {"replanning","replanningDetail","pendingEnvironmentId","environmentReleased","runStop","feedback"})
                current.erase(key);
            return true;
        });
    }

    void stop(Json task) {
        const bool cancel = task["state"] == "cancelling";
        const auto intent = cancel ? "cancel" : "replan";
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
            const auto copy = std::ranges::find_if(pending,[&](const Json& p) {
                return p.at("messageId") == item.at("messageId") && p.at("step") == item.at("step");
            });
            if (copy != pending.end()) pending.erase(copy);
            db.journal(task["id"],"instruction_delivered",{{"actionId",action["id"]},{"messageId",item["messageId"]},
                {"step",item.value("step",0)},
                {"revisionId",action.value("revisionId",Json(nullptr))},{"run",task["activeRun"]},
                {"source",item.value("source",Json(nullptr))}});
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

    enum class DeliveryOutcome { Accepted, DefinitelyUnsent, Unknown };

    void applyDelivery(Id id, const Json& action, Delivery outcome) {
        changeTask(id,[&](Json& task) {
            if (task["actionId"] != action["id"] || terminal(task)) return false;
            const auto scrub = db.redactor(field(task,"botId"));
            if (scrub.payload(runJson(outcome.run)) != runJson(outcome.run))
                outcome = {Delivery::Kind::Unknown,{},"T3 returned a protected Run identifier"};
            outcome.detail = scrub.text(outcome.detail);
            const bool cancelling = task["state"] == "cancelling";
            const bool reply = action["tool"] == "answer_session_question";
            DeliveryOutcome delivery = DeliveryOutcome::Unknown;
            if (outcome.kind == Delivery::Kind::Accepted) {
                if (reply || (hasRun(outcome.run) && assignedRunMatches(task,action,outcome.run)))
                    delivery = DeliveryOutcome::Accepted;
                else {
                    outcome.detail = "The returned Run does not identify the assigned work";
                    outcome.run = {};
                }
            } else if (outcome.definitelyUnsent || outcome.kind == Delivery::Kind::NotAccepted)
                delivery = DeliveryOutcome::DefinitelyUnsent;

            if (delivery == DeliveryOutcome::Accepted) {
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
                closeQuestion(task,"question_gone","resolved_or_expired");
                task["actionId"] = "";
                if (!cancelling) task["state"] = "running";
                return true;
            }
            if (delivery == DeliveryOutcome::Unknown && !outcome.run.threadId.empty() &&
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

    void deliver(const Json& task, const Json& action) {
        auto cmd = command(task,action);
        const auto id = task["id"].get<Id>();
        Delivery outcome;
        const auto launchAllowed = [&] {
            return (cmd.tool != "delegate" && cmd.tool != "follow_up") ||
                   access.permits(cmd.ownerBotId,{"environment",cmd.environmentId,"launch"},true);
        };
        const Delivery denied{Delivery::Kind::Settings,{},"Approve launch access in Access Settings (/settings/" +
            cmd.ownerBotId + "/environment) or use 'flock access grant BOT environment ENVIRONMENT_ID launch'.",true};
        const bool firstSend = action["phase"] == "ready";
        if (firstSend) {
            if (!launchAllowed()) return applyDelivery(id,action,denied);
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
            if (!change(task,[&](Json& current) {
                if (current["actionId"] != action["id"]) return false;
                db.exec("UPDATE outbox SET phase='started',auth_generation=? WHERE id=? AND phase='ready'",
                        {cmd.authGeneration,action["id"]});
                if (!sqlite3_changes(db.db)) return false;
                if (cmd.tool != "answer_session_question" && current["deadline"] == 0)
                    current["deadline"] = options.clock() + db.environmentTimeout(cmd.ownerBotId,cmd.environmentId,current.at("config"));
                db.journal(current["id"],"send_started",{{"actionId",action["id"]}});
                return true;
            })) return;
        } else if (action["phase"] != "started") return;
        try {
            if (!firstSend) outcome = t3.reconcile(cmd);
            else outcome = launchAllowed() ? t3.send(cmd) : denied;
        } catch (const std::exception&) {
            outcome.detail = "T3 communication failed; check the saved action before retrying";
        }
        applyDelivery(id,action,std::move(outcome));
    }

    bool hasSettledRun(const Json& task) const {
        return task["activeRun"].is_null() && hasRun(runRef(task["lastRun"]));
    }

    bool canComplete(const Json& task, const Json& event) const {
        if (task.value("replanning",false)) return false;
        if (!task.value("newSessionRequired",false) && hasSettledRun(task) &&
            task.at("lastRun").at("revisionId") == revision(task)) return true;
        // A peer result is actionable only for the sender and the closing message of that exchange.
        if (!event.contains("exchangeId") || !event.contains("status")) return false;
        auto exchange = db.query(R"SQL(
SELECT p.kind, recipient.environment_id
FROM peer_exchanges p JOIN tasks recipient ON recipient.id=p.recipient_task
WHERE p.id=? AND p.sender_task=? AND p.phase='closed' AND p.last_message=?
  AND p.sender_environment=? AND p.sender_revision IS ?
)SQL",{event.at("exchangeId"),task.at("id"),event.at("messageId"),task.at("environmentId"),revision(task)});
        if (!exchange.next()) return false;
        if (task.value("newSessionRequired",false))
            return exchange.string(1) == field(task,"environmentId") && event.at("status") == "completed";
        return task.contains("currentRevision") || (exchange.string(0) == "consult" && event.at("status") == "completed");
    }

    std::vector<std::string> available(const Json& task, const Json& event,
                                       const std::optional<PeerExchange>& peer, bool canSend, bool canRead) {
        const auto id = task.at("id").get<Id>();
        if (peer && peer->sender == id && peer->phase == "question") return {"reply_to_bot","ask_user"};
        if (!task["activeRun"].is_null() && !field(task["question"],"requestId").empty())
            return {"answer_session_question","ask_user"};
        const bool receiving = peer && peer->recipient == id && peer->phase == "working";
        if (receiving && peer->kind == "consult") return {"reply_to_bot"};
        std::vector<std::string> tools{"delegate",receiving ? "reply_to_bot" : "ask_user"};
        if (hasSettledRun(task)) {
            if (!task.value("newSessionRequired",false)) tools.push_back("follow_up");
            if (options.sessionRead == "on_demand" && event["evidence"].size() < 3) tools.push_back("session_read");
        }
        if (!receiving) {
            std::lock_guard lock(db.mutex);
            if (canComplete(task,event)) tools.push_back("complete");
        }
        if (canRead && event["evidence"].size() < 3) tools.push_back("read_bot_evidence");
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

    static constexpr int maxToolErrors = 6;

    void rejectDecision(const Json& task, const Json& event, const Decision& decision, std::string_view message) {
        change(task,[&](Json& current) {
            const auto why = bounded(message,2048);
            Json entry = {{"tool",bounded(decision.tool,128)},{"result",{{"ok",false},{"error",why}}},
                          {"eventId",event.at("id")}};
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
        if (!access.permits(field(task,"botId"),Scope::sessionRead(req.environmentId,req.threadId),true))
            throw Conflict("Session read access required");
        return persistExcerpt(task,event,readExcerpt(field(task,"botId"),req),"session_read",usage);
    }

    bool persistExcerpt(const Json& task, const Json& event, Json entry, std::string_view kind, const Json& usage) {
        return change(task,[&](Json& current) {
            auto evidence = event.at("evidence");
            if (evidence.size() >= 3) throw Conflict("History read allowance exhausted");
            evidence.push_back(entry);
            db.exec("UPDATE events SET evidence=? WHERE id=?", {evidence,event["id"]});
            if (!usage.empty()) entry["usage"] = usage;
            db.journal(current["id"],kind,entry);
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
            return PrefillResult::UnavailableWithSettledReport;
        }
    }

    struct WorkPrompt {
        std::string instruction;
        Json pending = Json::array(), contextIds = Json::object(), resultReferences = Json::array();
    };

    WorkPrompt workPrompt(const Json& task, const Json& event, std::size_t actionIdBytes) {
        WorkPrompt prompt;
        auto& [instruction,pending,contextIds,resultReferences] = prompt;
        // Use the same allowance before and after the model supplies its instruction and criteria.
        std::size_t overhead = 8000 + 512 + std::max<std::size_t>(64,actionIdBytes);
        if (event.contains("userAnswer")) {
            contextIds["answerEventId"] = event.at("id");
            instruction += "\n\nUser answer:\n" + field(event,"userAnswer");
        }
        const bool moved = task.value("newSessionRequired",false);
        if (moved || task["lastRun"].is_null() || task["lastRun"].at("revisionId") != revision(task)) {
            const auto criteria = task.value("requestedDoneWhen",field(task,"doneWhen"));
            instruction += "\n\nTask intent:\n" + taskIntent(task) +
                "\n\nCompletion criteria:\n" + criteria;
            overhead += std::max<std::size_t>(criteria.size(),8000) - criteria.size();
        }
        if (moved) {
            instruction += priorInstructions(task);
            if (event["kind"] == "bot_answer") {
                contextIds["peerMessageId"] = event.at("messageId");
                instruction += "\n\nAnswer from the requesting Bot:\n" + field(event,"text");
            }
            const auto& report = event.contains("sessionResult") ? event.at("sessionResult") : event;
            if (report.contains("report")) instruction += "\n\nPrevious Session report (reference only):\n" + field(report,"report");
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
        SourceQuotes quotes;
        const auto quoteSource = [&](const Json& source) {
            const auto quote = quotes.quote(source);
            if (quote.empty()) return;
            instruction += quote;
            auto reference = source;
            reference.erase("text");
            resultReferences.push_back(std::move(reference));
        };
        if (task.contains("sourceTaskId") && task["lastRun"].is_null()) {
            for (const auto& source : task.at("sourceResultRefs"))
                if (source.at("taskId") == task.at("sourceTaskId")) quoteSource(source);
        }
        overhead += instruction.size();
        pending = pendingBatch(task,overhead < maxSessionPromptBytes ? maxSessionPromptBytes - overhead : 0,quotes);
        includeInstructions(instruction,pending);
        for (const auto& item : pending)
            if (item.contains("source")) quoteSource(item.at("source"));
        return prompt;
    }

    void queueAction(Json& task, const std::string& action, std::string_view tool, std::string instruction,
                     const Json& args, const Json& event, bool charge) {
        WorkPrompt prompt;
        if (tool == "delegate" || tool == "follow_up") {
            prompt = workPrompt(task,event,action.size());
            instruction += prompt.instruction + "\n\n[Flock action ID: " + action + "]";
            task["deadline"] = 0;
        }
        const Json out = {{"id",action},{"tool",tool},{"environmentId",task["environmentId"]},
            {"target",tool == "follow_up" ? task["lastRun"] : task["activeRun"]},
            {"instruction",std::move(instruction)},{"questionId",field(args,"requestId")},
            {"answers",args.value("answers",Json::object())},{"revisionId",revision(task)},
            {"instructions",prompt.pending},{"contextIds",prompt.contextIds},{"sourceResultRefs",prompt.resultReferences},{"chargesBudget",charge}};
        db.exec("INSERT INTO outbox(id,task_id,phase,data) VALUES(?,?,'ready',?)", {action,task["id"],out});
        task["actionId"] = action;
        task["state"] = "sending";
    }

    bool admitWork(Json& task, const Decision& decision, bool& charge) {
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

    std::string priorInstructions(const Json& task) {
        auto rows = db.query(R"SQL(
SELECT entry.value FROM outbox, json_each(outbox.data,'$.instructions') entry
WHERE outbox.task_id=? AND outbox.phase='accepted'
ORDER BY outbox.rowid DESC, entry.key DESC LIMIT 20
)SQL", {task["id"]});
        std::vector<std::string> excerpts;
        std::size_t remaining = 4000;
        while (remaining && rows.next()) {
            const auto item = rows.json(0);
            auto text = bounded(field(item,"text"),remaining);
            remaining -= text.size();
            excerpts.push_back(std::move(text));
        }
        if (excerpts.empty()) return {};
        std::string context = "\n\nEarlier user directions from this Task's prior Session (excerpts):";
        for (auto item = excerpts.rbegin(); item != excerpts.rend(); ++item) context += "\n\n" + *item;
        return context;
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

    void recordDecision(const Json& task, const Json& event, const Decision& decision, std::string_view action) {
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

    void sendPeer(Json& sender, const Json& event, const Decision& decision, PeerBot peer, const std::string& action) {
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
            if (!admitWork(sender,decision,charge)) return;
            establishCriteria(sender,args);
            ensureNoRouteCycle(sender,peer.intake);
        }
        peer.intake.request = note.empty() ? field(forwarded,"text") : note;
        peer.intake.scheduleId.clear();
        peer.intake.scheduledAt = 0;
        const auto id = db.submit(peer.intake,0,nullptr,RequestJournal::skip);
        auto recipient = db.load(id);
        const PeerExchange exchange{action,kind,"working","",sender["id"],id};
        auto message = peerMessage(exchange,sender,recipient,"request",note,action);
        message["chargesBudget"] = charge;
        if (!forwarded.is_null()) message["forwardedMessage"] = std::move(forwarded);
        transitionPeer(exchange,sender,recipient,std::move(message));
        recordDecision(sender,event,decision,action);
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
        return {{"environmentId",report.at("environmentId")},{"threadId",run.threadId},{"runId",field(report,"resultRunId")},
                {"itemId",field(report,"itemId")},{"link",run.link},{"status",field(report,"status")}};
    }

    void finishPeer(const PeerExchange& exchange, Json& recipient, Json sender, std::string_view status,
                    std::string_view text, const std::string& action) {
        const auto type = status == "completed" ? std::string_view{"result"} : status;
        auto message = peerMessage(exchange,recipient,sender,type,text,action);
        message["status"] = status;
        if (status != "cancelled") {
            const auto ref = resultReference(recipient);
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
            kinds.push_back("result");
        }
        return kinds;
    }

    bool replyPeer(Json& task, const Json& event, const Decision& decision, const std::string& action) {
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

    void readPeerEvidence(const Json& task, const Json& event, const Decision& decision) {
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
        persistExcerpt(task,event,std::move(entry),"read_bot_evidence",decision.usage);
    }

    void acceptReplyOrCompletion(const Json& snapshot, const Json& event, const Decision& decision) {
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

    void complete(Json& current, const Json& event, const Decision& decision) {
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
        db.journal(current["id"],"completed",{{"text",field(decision.args,"report")},{"reason",decision.reason},
            {"doneWhen",current["doneWhen"]},{"revisionId",revision(current)},
            {"source",{{"environmentId",field(event,"environmentId")},
                {"threadId",field(event.value("run",Json(nullptr)),"threadId")},
                {"runId",field(event,"resultRunId")},{"itemId",field(event,"itemId")},
                {"link",field(event.value("run",Json(nullptr)),"link")}}}});
        saveTask(current);
    }

    void accept(const Json& snapshot, const Json& event, const Decision& decision) {
        const auto& args = decision.args;
        const auto& tool = decision.tool;
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
            auto peer = allowedPeer(snapshot,field(args,"botId"));
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
                auto& owner = current["question"].is_object() ? current["question"] : current["wait"];
                owner["target"] = questionTarget(current);
            } else {
                queueAction(current,action,tool,instruction,args,event,charge);
            }
            return true;
        });
    }

    bool questionStillOpen(const Json& task) {
        const auto observed = observe(task);
        if (ingest(task,observed) || observed.kind != Observation::Kind::Question) return false;
        const auto target = questionTarget(task);
        const auto& question = task["question"];
        return question.is_object() && question.value("target",target) == target &&
            target["requestId"] == observed.question.requestId;
    }

    std::optional<Decision> savedDecision(const Json& task, const Json& event) {
        auto saved = task.find("pendingDecision");
        if (saved == task.end() || saved->at("eventId") != event.at("id") ||
            saved->at("generation") != task.at("generation") || saved->at("revisionId") != revision(task))
            return std::nullopt;
        return Decision{saved->at("tool"),saved->at("args"),saved->at("reason"),saved->at("usage")};
    }

    std::optional<Decision> inferDecision(const Json& task, Json event, const Json& history,
            const std::vector<std::string>& tools, const Json& pending, Json choices,
            const std::optional<PeerExchange>& peer) {
        const auto prefill = prefillHistory(task,event);
        if (prefill == PrefillResult::Persisted) return std::nullopt;
        if (prefill == PrefillResult::UnavailableWithSettledReport) event["historyUnavailable"] = true;
        const auto bot = notes(field(task,"botId"));
        Json summary = {{"id",task["id"]},{"botId",task["botId"]},{"request",taskIntent(task)},
            {"doneWhen",task["doneWhen"]},
            {"remaining",task["remaining"]},{"additionalUsed",task["additionalUsed"]},{"revisionId",revision(task)}};
        if (task.contains("sourceMessageId")) summary["sourceMessageId"] = task.at("sourceMessageId");
        if (task.contains("requestedDoneWhen")) summary["requestedDoneWhen"] = task.at("requestedDoneWhen");
        if (task.contains("restartContext")) summary["restartContext"] = task.at("restartContext");
        if (hasPending(task)) {
            summary["userInstructions"] = pending;
            summary["pendingInstructionCount"] = task.at("pendingInstructions").size();
        }
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
        summary["remainingEvidenceReads"] = canReadHistory ? 3 - evidence.size() : 0;
        event.erase("evidence");
        event.erase("consumed");
        Id attempt = 0;
        {
            std::lock_guard lock(db.mutex);
            Transaction transaction(db.db);
            auto current = db.load(task["id"]);
            if (current["generation"] != task["generation"] || current["eventId"] != event["id"] ||
                revision(current) != revision(task) || current["state"] != "deciding")
                return std::nullopt;
            const auto budget = db.taskModelBudget(current);
            attempt = db.reserveModelCall(budget,"task_decision",0,task["id"],event["id"]);
            if (!attempt) {
                db.wait(current,"model_budget",{{"text",
                    "The model call limit is exhausted. Approve one more model call to continue."}});
                saveTask(current);
            }
            transaction.commit();
        }
        if (!attempt) return std::nullopt;
        const auto decision = db.callModel(field(task,"botId"),model,
            {summary,event,history,bot,tools,field(task,"feedback"),evidence,task["config"],
             needsDoneWhen(task),summary.contains("forwardableUserMessage")},attempt);
        // Bookkeeping must not advance the generation observed by conversation turns.
        const Json saved = {{"generation",task["generation"]},{"eventId",event["id"]},{"revisionId",revision(task)},
            {"tool",decision.tool},{"args",decision.args},{"reason",decision.reason},{"usage",decision.usage}};
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        db.exec("UPDATE tasks SET data=json_set(data,'$.pendingDecision',json(?))"
                " WHERE id=? AND state='deciding' AND generation=? AND json_extract(data,'$.eventId')=?",
                {saved,task["id"],task["generation"],event["id"]});
        const bool savedForEvent = sqlite3_changes(db.db) != 0;
        transaction.commit();
        if (!savedForEvent) return std::nullopt;
        return decision;
    }

    void decide(const Json& task) {
        if (!task["activeRun"].is_null() && !questionStillOpen(task)) return;
        Json event, history = Json::array();
        std::optional<PeerExchange> peer;
        std::optional<Decision> decision;
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
                for (const auto& allowed : peersFor(task)) {
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
            if (hasPending(task) && task.at("pendingInstructions").size() != pending.size())
                std::erase(tools,"complete");
            if (!decision)
                decision = inferDecision(task,event,history,tools,pending,std::move(choices),peer);
            if (!decision) return;
            if (!task["activeRun"].is_null() && !questionStillOpen(task)) return;
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

    void step(Id id) {
        const auto task = snapshot(id);
        const auto state = field(task,"state");
        if (shouldStop(task)) stop(task);
        else if (state == "queued") activate(task);
        else if (state == "deciding") decide(task);
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
        if (pending.empty() || !p_->db.runTurn(pending.front().second,p_->model,p_->t3,p_->notes)) break;
    }
}
void Runtime::tick() {
    for (const auto& [bot,id] : p_->db.pendingConversations(Impl::batchLimit))
        p_->db.runTurn(id,p_->model,p_->t3,p_->notes);
    for (auto id : p_->nextProjects(Impl::batchLimit)) p_->step(id);
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
    constexpr auto maxConversationWorkers = Impl::batchLimit;
    if (p_->conversationWorkers.size() < maxConversationWorkers) {
        for (const auto& [bot,id] : p_->db.pendingConversations(maxConversationWorkers)) {
            if (p_->conversationWorkers.size() >= maxConversationWorkers) break;
            if (!p_->conversationWorkers.contains(bot)) {
                p_->conversationWorkers.emplace(bot,std::async(std::launch::async,[this,id] {
                    p_->db.runTurn(id,p_->model,p_->t3,p_->notes);
                }));
            }
        }
    }
    for (auto id : p_->nextProjects(Impl::batchLimit - p_->workers.size()))
        p_->workers.emplace(id,std::async(std::launch::async,[this,id] { p_->step(id); }));
}
} // namespace flockv0
