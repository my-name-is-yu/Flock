#include "storage.hpp"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <future>
#include <map>
#include <set>
#include <sys/file.h>
#include <unistd.h>

namespace flockv0 {
namespace {
bool present(std::string_view value, std::size_t limit = 8000) {
    return value.size() <= limit && value.find_first_not_of(" \r\n\t") != std::string_view::npos;
}
bool hasRun(const RunRef& r) { return !r.threadId.empty() && !r.runId.empty(); }

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
    Model& model;
    T3& t3;
    NotesLoader notes;
    RuntimeOptions options;
    int lockFd = -1;
    std::map<Id,std::future<void>> workers;

    Impl(Store::Impl& store, Model& m, T3& adapter, NotesLoader loader, RuntimeOptions opts)
        : db(store), model(m), t3(adapter), notes(std::move(loader)), options(std::move(opts)) {
        const auto lockPath = std::filesystem::canonical(db.path).string() + ".runtime.lock";
        lockFd = open(lockPath.c_str(), O_CLOEXEC | O_CREAT | O_RDWR, 0600);
        if (lockFd < 0 || ::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
            if (lockFd >= 0) close(lockFd);
            throw Conflict("Cannot acquire the exclusive Runtime lock for this database");
        }
    }
    ~Impl() {
        workers.clear();
        close(lockFd);
    }

    template<class F> bool change(const Json& snapshot, F fn) {
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        auto task = db.load(snapshot["id"]);
        if (task["generation"] != snapshot["generation"] || !fn(task)) return false;
        db.save(task);
        transaction.commit();
        return true;
    }

    std::vector<Id> eligible() {
        std::lock_guard lock(db.mutex);
        auto rows = db.query(R"SQL(
SELECT id FROM project_task_order WHERE position=1
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
            if (current["eventId"] == 0) db.event(current,"intake",{{"kind","intake"}});
            current["state"] = "deciding";
            return true;
        });
    }

    void cancelDone(Json& task) {
        db.clearWait(task,"cancelled");
        db.unlockQuestion(task,"cancelled");
        if (!field(task,"actionId").empty()) {
            db.exec("UPDATE outbox SET phase='closed' WHERE id=? AND phase='ready'", {task["actionId"]});
            task["actionId"] = "";
        }
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
            if (observed.kind == Observation::Kind::Finished && observed.settled) {
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
                task["activeRun"] = nullptr;
                task["lastRun"] = runJson(run);
                db.consumeEvent(task);
                const auto report = bounded(observed.report,8192);
                const Json data = {{"kind","result"},{"run",runJson(run)},
                    {"resultRunId",observed.resultRunId.empty() ? run.runId : observed.resultRunId},
                    {"status",observed.status},{"detail",bounded(observed.detail,2048)},
                    {"report",report},{"truncated",observed.truncated || report.size() < observed.report.size()},
                    {"environmentId",task["environmentId"]},{"itemId",observed.itemId}};
                db.exec("INSERT OR IGNORE INTO t3_events VALUES(?,?,'result',?)",{task["environmentId"],run.threadId,run.runId});
                if (sqlite3_changes(db.db)) {
                    if (state != "cancelling") {
                        task.erase("feedback");
                        db.event(task,"result/" + run.threadId + "/" + run.runId,data);
                    }
                    db.journal(task["id"],"report",data);
                }
                if (state == "cancelling") cancelDone(task);
                else task["state"] = "deciding";
                return true;
            }
            if (state == "cancelling") return false;
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
                db.event(task,"question/" + run.threadId + "/" + observed.question.requestId,data);
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
            return t3.observe(field(task,"environmentId"),task["config"],runRef(task["activeRun"]));
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

    Command command(const Json& task, const Json& action) {
        Command c;
        c.actionId = field(action,"id");
        c.tool = field(action,"tool");
        c.environmentId = field(task,"environmentId");
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

    void applyDelivery(const Json& snapshotTask, const Json& action, Delivery outcome) {
        // A concurrent cancel invalidates a decision, but an accepted side effect still belongs to its task.
        const auto current = snapshot(snapshotTask["id"]);
        if (current["actionId"] != action["id"] || terminal(current)) return;
        change(current,[&](Json& task) {
            const bool cancelling = task["state"] == "cancelling";
            const bool reply = action["tool"] == "answer_session_question";
            const auto id = task["id"].get<Id>();
            if (outcome.kind == Delivery::Kind::Accepted && !reply && hasRun(outcome.run)) {
                const auto target = runRef(action["target"]);
                auto owner = db.query("SELECT action_id FROM runs WHERE environment_id=? AND thread_id=? AND run_id=?",
                    {task["environmentId"],outcome.run.threadId,outcome.run.runId});
                if ((action["tool"] == "follow_up" &&
                     (outcome.run.threadId != target.threadId || outcome.run.runId == target.runId)) ||
                    (owner.next() && owner.string(0) != field(action,"id"))) {
                    outcome.kind = Delivery::Kind::Unknown;
                    outcome.detail = "The returned Run does not match the assigned work";
                }
            }
            if (outcome.kind == Delivery::Kind::Accepted && (reply || hasRun(outcome.run))) {
                db.exec("UPDATE outbox SET phase='accepted' WHERE id=?", {action["id"]});
                if (reply) db.unlockQuestion(task,"answered");
                else {
                    db.exec("INSERT OR IGNORE INTO runs VALUES(?,?,?,?,?,?)", {action["id"],id,
                        task["environmentId"],outcome.run.threadId,outcome.run.runId,outcome.run.link});
                    task["activeRun"] = runJson(outcome.run);
                    db.journal(id,"run_started",{{"run",task["activeRun"]},{"actionId",action["id"]}});
                }
                if (reply) db.journal(id,"session_answered",{{"requestId", action["questionId"]},{"actionId",action["id"]}});
                task["actionId"] = "";
                if (!cancelling) task["state"] = "running";
                return true;
            }
            if (outcome.kind == Delivery::Kind::QuestionGone && reply) {
                db.exec("UPDATE outbox SET phase='closed' WHERE id=?", {action["id"]});
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
            } else db.wait(task,reason,{{"text",detail}});
            return true;
        });
    }

    void deliver(const Json& task, const Json& action) {
        auto cmd = command(task,action);
        Delivery outcome;
        try {
            if (action["phase"] == "started") {
                outcome = t3.reconcile(cmd);
            } else {
                if (const auto stop = t3.preflight(cmd)) {
                    applyDelivery(task,action,*stop);
                    return;
                }
                cmd.authGeneration = t3.authGeneration(cmd.environmentId,cmd.config);
                if (!change(task,[&](Json& current) {
                    db.exec("UPDATE outbox SET phase='started',auth_generation=? WHERE id=?", {cmd.authGeneration,action["id"]});
                    if (cmd.tool != "answer_session_question" && current["deadline"] == 0)
                        current["deadline"] = options.clock() + current["config"].value("runTimeoutSeconds",1800);
                    db.journal(current["id"],"send_started",{{"actionId",action["id"]}});
                    return true;
                })) return;
                outcome = t3.send(cmd);
            }
        } catch (const std::exception&) {
            outcome.detail = "T3 communication failed; check the saved action before retrying";
        }
        applyDelivery(task,action,std::move(outcome));
    }

    std::vector<std::string> available(const Json& task, const Json& event,
                                       const std::optional<PeerExchange>& peer, bool canSend, bool canRead) {
        if (event["kind"] == "bot_question") return {"reply_to_bot","ask_user"};
        if (peer && peer->recipient == task["id"]) {
            if (event["kind"] == "question") return {"answer_session_question","ask_user"};
            if (peer->kind == "consult") return {"reply_to_bot","ask_user"};
            if (event["kind"] == "result" || event.contains("sessionResult")) {
                std::vector<std::string> tools{"delegate","follow_up","reply_to_bot","ask_user"};
                if (options.sessionRead == "on_demand" && event["evidence"].size() < 3) tools.push_back("session_read");
                return tools;
            }
            return {"delegate","reply_to_bot","ask_user"};
        }
        if (event["kind"] == "question") return {"answer_session_question","ask_user"};
        std::vector<std::string> tools{"delegate","ask_user"};
        if ((event["kind"] == "result" || event.contains("sessionResult")) && !task["lastRun"].is_null()) {
            tools.push_back("follow_up");
            if (options.sessionRead == "on_demand" && event["evidence"].size() < 3) tools.push_back("session_read");
        }
        if (event["kind"] == "result" && !task["lastRun"].is_null()) tools.push_back("complete");
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
            const auto why = bounded(detail,2048);
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

    Json readExcerpt(const ReadRequest& req) {
        const auto excerpt = t3.read(req);
        if ((!excerpt.threadId.empty() && excerpt.threadId != req.threadId) ||
            (!excerpt.runId.empty() && excerpt.runId != req.runId))
            throw Conflict("History source does not match the requested Run");
        const auto text = bounded(excerpt.text,8192);
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
        const auto assigned = runRef(report.value("run",Json::object()));
        bool owned = assigned.threadId == req.threadId &&
            (assigned.runId == req.runId || field(report,"resultRunId") == req.runId);
        if (!owned) {
            std::lock_guard lock(db.mutex);
            auto row = db.query("SELECT 1 FROM runs WHERE task_id=? AND environment_id=? AND thread_id=? AND run_id=?",
                                {task["id"],req.environmentId,req.threadId,req.runId});
            owned = row.next();
        }
        if (!owned || req.threadId.empty() || req.runId.empty()) throw Conflict("History reference does not belong to this task");
        auto entry = readExcerpt(req);
        return change(task,[&](Json& current) {
            auto evidence = event["evidence"];
            evidence.push_back(entry);
            db.exec("UPDATE events SET evidence=? WHERE id=?", {evidence,event["id"]});
            if (!usage.empty()) entry["usage"] = usage;
            db.journal(current["id"],"session_read",entry);
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

    void recordDecision(const Json& task, const Json& event, const Decision& decision, std::string_view action) {
        db.journal(task["id"],"decision",{{"actionId",action},{"tool",decision.tool},{"args",decision.args},
            {"reason",decision.reason},{"eventId",event["id"]},{"evidence",event["evidence"]},{"usage",decision.usage}});
    }

    void sendPeer(Json& sender, const Json& event, const Decision& decision, PeerBot peer, const std::string& action) {
        if (db.peerExchange(sender["id"])) throw Conflict("Finish the current Bot exchange first");
        const auto kind = field(decision.args,"kind");
        const bool work = kind == "work";
        const bool initial = field(sender,"doneWhen").empty();
        if (work) {
            if (!sender["activeRun"].is_null() || !field(sender,"actionId").empty())
                throw Conflict("Let the assigned Run settle before requesting another Bot's work");
            if (initial) sender["doneWhen"] = field(decision.args,"doneWhen");
            else if (sender["remaining"] == 0) {
                db.wait(sender,"budget",{{"text","The additional work allowance is exhausted"},
                    {"attempted",decision.tool},{"decisionReason",decision.reason},{"usage",decision.usage}});
                return;
            } else {
                sender["remaining"] = sender["remaining"].get<int>() - 1;
                sender["additionalUsed"] = sender["additionalUsed"].get<int>() + 1;
            }
            ensureNoRouteCycle(sender,peer.intake);
        }
        peer.intake.request = field(decision.args,"text");
        peer.intake.scheduleId.clear();
        peer.intake.scheduledAt = 0;
        const auto id = db.submit(peer.intake,kind);
        auto recipient = db.load(id);
        const PeerExchange exchange{action,kind,"working","",sender["id"],id};
        db.exec("INSERT INTO peer_exchanges VALUES(?,?,'working',?,?,?)",{action,kind,action,sender["id"],id});
        auto message = peerMessage(exchange,sender,recipient,"request",peer.intake.request,action);
        db.peerMessage(exchange,message);
        peerEvent(recipient,std::move(message),"bot_request");
        db.save(recipient);
        db.consumeEvent(sender);
        sender.erase("feedback");
        recordDecision(sender,event,decision,action);
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
        return {{"environmentId",task["environmentId"]},{"threadId",run.threadId},{"runId",field(report,"resultRunId")},
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

    void replyPeer(Json& task, const Json& event, const Decision& decision, const std::string& action) {
        const auto peer = db.peerExchange(task["id"]);
        if (!peer) throw Conflict("The Bot exchange has closed");
        const auto type = field(decision.args,"kind");
        const auto body = field(decision.args,"text");
        const bool answering = type == "answer" && peer->sender == task["id"] && peer->phase == "question";
        const bool receiving = peer->recipient == task["id"] && peer->phase == "working";
        if (!answering && !(receiving && (type == "question" || type == "result" || type == "failed")))
            throw Conflict("This reply does not match the current Bot exchange");
        const auto otherId = answering ? peer->recipient : peer->sender;
        auto other = db.load(otherId);
        if (terminal(other) || other["state"] != "peer_waiting") throw Conflict("The other Bot is no longer waiting");
        db.consumeEvent(task);
        task.erase("feedback");
        recordDecision(task,event,decision,action);
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
            auto row = db.query("SELECT recipient_task FROM peer_exchanges WHERE id=? AND sender_task=? AND last_message=?",
                                {event["exchangeId"],task["id"],messageId});
            if (!row.next()) throw Conflict("The Bot result does not belong to this task");
            req.config = db.load(row.number(0))["config"];
        }
        if (req.threadId.empty() || req.runId.empty()) throw Conflict("The Bot result has no readable Run");
        auto entry = readExcerpt(req);
        entry["messageId"] = messageId;
        entry["exchangeId"] = event["exchangeId"];
        change(task,[&](Json& current) {
            auto evidence = event["evidence"];
            evidence.push_back(entry);
            db.exec("UPDATE events SET evidence=? WHERE id=?",{evidence,event["id"]});
            if (!decision.usage.empty()) entry["usage"] = decision.usage;
            db.journal(current["id"],"read_bot_evidence",entry);
            return true;
        });
    }

    void accept(const Json& snapshot, const Json& event, const Decision& decision) {
        const auto& args = decision.args;
        const auto& tool = decision.tool;
        static const std::map<std::string_view,std::set<std::string_view>> arguments{
            {"delegate",{"instruction","doneWhen"}},
            {"answer_session_question",{"requestId","answers"}},
            {"follow_up",{"instruction"}},
            {"complete",{"report"}},
            {"ask_user",{"question","choices"}},
            {"session_read",{"threadId","runId","itemId","cursor","view","offset"}},
            {"send_bot_message",{"botId","kind","text","doneWhen"}},
            {"reply_to_bot",{"kind","text"}},
            {"read_bot_evidence",{"messageId","itemId","cursor","offset"}}
        };
        for (auto item = args.begin(); item != args.end(); ++item) {
            if (!arguments.at(tool).contains(item.key())) throw Conflict("Unknown action argument");
        }
        if (tool == "session_read") {
            readEvidence(snapshot,event,args,decision.usage);
            return;
        }
        if (tool == "read_bot_evidence") return readPeerEvidence(snapshot,event,decision);
        if (tool == "send_bot_message" || tool == "reply_to_bot") {
            if (!present(field(args,"text"))) throw Conflict("A Bot message requires text");
            if (tool == "reply_to_bot") {
                change(snapshot,[&](Json& current) { replyPeer(current,event,decision,actionId()); return true; });
                return;
            }
            const auto kind = field(args,"kind");
            if (kind != "consult" && kind != "work") throw Conflict("Choose a consultation or work request");
            const bool requiresCondition = kind == "work" && field(snapshot,"doneWhen").empty();
            if ((requiresCondition && !present(field(args,"doneWhen"))) || (!requiresCondition && args.contains("doneWhen")))
                throw Conflict("Only the first work request sets the task's acceptance criteria");
            auto peer = allowedPeer(snapshot,field(args,"botId"));
            change(snapshot,[&](Json& current) { sendPeer(current,event,decision,std::move(peer),actionId()); return true; });
            return;
        }
        const bool work = tool == "delegate" || tool == "follow_up";
        const bool initial = field(snapshot,"doneWhen").empty();
        const auto instruction = field(args,"instruction");
        const auto doneWhen = field(args,"doneWhen");
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
        if (tool == "complete" && !present(field(args,"report"))) throw Conflict("A completion report is required");
        if (tool == "answer_session_question") {
            if (field(args,"requestId") != field(snapshot["question"],"requestId"))
                throw Conflict("The Session question no longer matches");
            validateAnswers(snapshot["question"]["fields"],args.value("answers",Json::object()));
        }
        change(snapshot,[&](Json& current) {
            const auto id = current["id"].get<Id>();
            if (work && !initial && current["remaining"] == 0) {
                db.wait(current,"budget",{{"text","The additional work allowance is exhausted"},{"attempted",tool},
                                          {"decisionReason",decision.reason},{"usage",decision.usage}});
                return true;
            }
            db.consumeEvent(current);
            const auto action = actionId();
            recordDecision(current,event,decision,action);
            current.erase("feedback");
            if (tool == "ask_user") {
                db.wait(current,"user_question",{{"text",field(args,"question")},{"decisionReason",decision.reason},
                    {"questionId",field(current["question"],"requestId")},{"choices",args.value("choices",Json::array())}});
            } else if (tool == "complete") {
                current["state"] = "completed";
                db.journal(id,"completed",{{"text",field(args,"report")},{"reason",decision.reason},{"doneWhen",current["doneWhen"]}});
            } else {
                if (initial) current["doneWhen"] = doneWhen;
                if (work && !initial) {
                    current["remaining"] = current["remaining"].get<int>() - 1;
                    current["additionalUsed"] = current["additionalUsed"].get<int>() + 1;
                }
                auto destination = tool == "follow_up" ? current["lastRun"] : current["activeRun"];
                const Json out = {{"id",action},{"tool",tool},{"target",destination},
                    {"instruction",work ? instruction + "\n\n[Flock action ID: " + action + "]" : ""},
                    {"questionId",field(args,"requestId")},{"answers",args.value("answers",Json::object())}};
                db.exec("INSERT INTO outbox(id,task_id,phase,data) VALUES(?,?,'ready',?)",{action,id,out});
                current["actionId"] = action;
                current["state"] = "sending";
                if (work) current["deadline"] = 0;
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
        {
            std::lock_guard lock(db.mutex);
            event = db.currentEvent(task);
            if (event.is_null() || event["consumed"] == true || event["retryAt"].get<Seconds>() > options.clock()) return;
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
        try {
            auto choices = Json::array();
            bool canRead = false;
            if (!peer && task["activeRun"].is_null()) {
                for (const auto& allowed : peersFor(task)) {
                    choices.push_back({{"botId",allowed.intake.botId},{"name",allowed.name}});
                    if (event.contains("references") && !event["references"].empty() && event["fromBotId"] == allowed.intake.botId)
                        canRead = true;
                }
            }
            const auto tools = available(task,event,peer,!choices.empty(),canRead);
            if (options.sessionRead == "prefill" && event["evidence"].empty() &&
                (event["kind"] == "result" || event.contains("sessionResult"))) {
                const auto& report = event.contains("sessionResult") ? event["sessionResult"] : event;
                const auto run = runRef(report["run"]);
                try {
                    readEvidence(task,event,{{"threadId",run.threadId},{"runId",run.runId},{"view","activity"}});
                    return;
                } catch (const std::exception&) {
                    if (!event.contains("sessionResult")) throw;
                    // Peer replies already carry a settled report and can proceed without a fresh excerpt.
                }
            }
            auto bot = notes(field(task,"botId"));
            bot.soul = bounded(bot.soul,8192);
            bot.memory = bounded(bot.memory,8192);
            Json summary = {{"id",task["id"]},{"botId",task["botId"]},{"request",task["request"]},{"doneWhen",task["doneWhen"]},
                            {"remaining",task["remaining"]},{"additionalUsed",task["additionalUsed"]}};
            if (!choices.empty()) summary["peers"] = std::move(choices);
            if (peer) summary["peer"] = {{"exchangeId",peer->id},{"requestKind",peer->kind},
                {"role",peer->sender == task["id"] ? "sender" : "recipient"}};
            const auto evidence = event["evidence"];
            event.erase("evidence");
            event.erase("consumed");
            const Decision decision = model.decide({summary,event,history,bot,tools,field(task,"feedback"),evidence,task["config"]});
            event["evidence"] = evidence;
            if (!task["activeRun"].is_null()) {
                const auto observed = observe(task);
                if (ingest(task,observed) || observed.kind != Observation::Kind::Question) return;
            }
            if (!decision.args.is_object() || decision.args.dump().size() > 20000 || !present(decision.reason,4000) ||
                std::find(tools.begin(),tools.end(),decision.tool) == tools.end())
                throw Conflict("Return exactly one permitted action, valid arguments and a reason");
            accept(task,event,decision);
        } catch (const ModelError& e) {
            failure(task,event,e.what(),e.transient);
        } catch (const std::exception& e) {
            failure(task,event,e.what(),false);
        }
    }

    void step(Id id) {
        const auto task = snapshot(id);
        const auto state = field(task,"state");
        if (state == "queued") activate(task);
        else if (state == "deciding") decide(task);
        else if (state == "cancelling") cancel(task);
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
    : p_(std::make_unique<Impl>(*store.p_,model,t3,std::move(notes),std::move(options))) {}
Runtime::~Runtime() = default;
void Runtime::tick() { for (auto id : p_->eligible()) p_->step(id); }
void Runtime::tickConcurrent() {
    for (auto it = p_->workers.begin(); it != p_->workers.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto result = std::move(it->second);
            it = p_->workers.erase(it);
            result.get();
        } else ++it;
    }
    for (auto id : p_->eligible()) {
        if (!p_->workers.contains(id))
            p_->workers.emplace(id,std::async(std::launch::async,[this,id] { p_->step(id); }));
    }
}
} // namespace flockv0
