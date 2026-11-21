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
SELECT id FROM tasks AS ready
WHERE state NOT IN ('completed','cancelled') AND NOT EXISTS (
  SELECT 1 FROM tasks AS prior WHERE prior.environment_id=ready.environment_id
  AND prior.project_id=ready.project_id AND prior.id<ready.id
  AND prior.state NOT IN ('completed','cancelled')
) ORDER BY id)SQL");
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
            db.event(current,"intake",{{"kind","intake"}});
            current["state"] = "deciding";
            return true;
        });
    }

    void cancelDone(Json& task) {
        db.clearWait(task,"cancelled");
        db.unlockQuestion(task,"cancelled");
        task["activeRun"] = nullptr;
        task["state"] = "cancelled";
        db.journal(task["id"],"cancelled",Json::object());
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
                task["activeRun"] = nullptr;
                task["lastRun"] = runJson(run);
                if (state == "cancelling") {
                    cancelDone(task);
                    return true;
                }
                db.consumeEvent(task);
                const auto report = bounded(observed.report,8192);
                const Json data = {{"kind","result"},{"run",runJson(run)},
                    {"resultRunId",observed.resultRunId.empty() ? run.runId : observed.resultRunId},
                    {"status",observed.status},{"report",report},{"truncated",observed.truncated || report.size() < observed.report.size()},
                    {"environmentId",task["environmentId"]},{"itemId",observed.itemId}};
                db.exec("INSERT OR IGNORE INTO t3_events VALUES(?,?,'result',?)",{task["environmentId"],run.threadId,run.runId});
                if (sqlite3_changes(db.db)) {
                    db.event(task,"result/" + run.threadId + "/" + run.runId,data);
                    db.journal(task["id"],"report",data);
                }
                task["state"] = "deciding";
                return true;
            }
            if (state == "cancelling") return false;
            if (observed.kind == Observation::Kind::Question) {
                if (observed.question.requestId.empty()) return false;
                if (field(task["question"],"requestId") == observed.question.requestId) return false;
                db.exec("INSERT OR IGNORE INTO t3_events VALUES(?,?,'question',?)",
                        {task["environmentId"],run.threadId,observed.question.requestId});
                if (!sqlite3_changes(db.db)) return false;
                db.clearWait(task,"session_question");
                db.unlockQuestion(task,"replaced");
                db.consumeEvent(task);
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
                if (observed.status != "auth" || reason == "auth") return false;
                db.wait(task,"auth",{{"text","T3 authentication is unavailable"}});
                return true;
            }
            if (reason == "t3_input" || !task["question"].is_null()) {
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
            action = db.outbox(task);
        }
        if (!action.is_null() && action["phase"] == "started") {
            deliver(task,action);
            return;
        }
        if (task["activeRun"].is_null()) {
            change(task,[&](Json& current) { cancelDone(current); return true; });
            return;
        }
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
            if (outcome.kind == Delivery::Kind::Accepted && (reply || hasRun(outcome.run))) {
                db.exec("UPDATE outbox SET phase='accepted' WHERE id=?", {action["id"]});
                if (reply) db.unlockQuestion(task,"answered");
                else {
                    db.exec("INSERT OR IGNORE INTO runs VALUES(?,?,?,?,?,?)", {action["id"],id,
                        task["environmentId"],outcome.run.threadId,outcome.run.runId,outcome.run.link});
                    auto owner = db.query("SELECT task_id,action_id FROM runs WHERE environment_id=? AND thread_id=? AND run_id=?",
                        {task["environmentId"],outcome.run.threadId,outcome.run.runId});
                    if (!owner.next() || owner.number(0) != id || owner.string(1) != field(action,"id"))
                        throw Conflict("T3 Run already belongs to another action");
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
            const auto reason = outcome.kind == Delivery::Kind::Auth ? "auth" :
                                outcome.kind == Delivery::Kind::Settings ? "settings" : "unknown_delivery";
            const auto detail = outcome.detail.empty() ? "The assigned side effect could not be confirmed" :
                                                         bounded(outcome.detail,2048);
            if (outcome.kind == Delivery::Kind::NotAccepted) {
                db.exec("UPDATE outbox SET phase=? WHERE id=?", {cancelling ? "closed" : "ready",action["id"]});
                if (cancelling) task["actionId"] = "";
            }
            if (cancelling) {
                if (field(task,"cancellationDetail") == detail && outcome.kind != Delivery::Kind::NotAccepted) return false;
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

    std::vector<std::string> available(const Json& task, const Json& event) {
        if (event["kind"] == "question") return {"answer_session_question","ask_user"};
        if (event["kind"] == "result" && !task["lastRun"].is_null()) {
            std::vector<std::string> tools{"delegate","follow_up","complete","ask_user"};
            if (options.sessionRead == "on_demand" && event["evidence"].size() < 3) tools.push_back("session_read");
            return tools;
        }
        return {"delegate","ask_user"};
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

    bool readEvidence(const Json& task, const Json& event, const Json& args) {
        ReadRequest req;
        req.environmentId = field(task,"environmentId");
        req.config = task["config"];
        req.threadId = field(args,"threadId");
        req.runId = field(args,"runId");
        req.itemId = field(args,"itemId");
        req.cursor = field(args,"cursor");
        req.view = field(args,"view").empty() ? "messages" : field(args,"view");
        if (args.contains("offset") && (!args["offset"].is_number_integer() || args["offset"].get<Id>() < 0))
            throw Conflict("Invalid history offset");
        req.offset = args.value("offset",std::size_t{0});
        if (req.view != "messages" && req.view != "activity") throw Conflict("Unsupported history view");
        const auto active = runRef(event.value("run",Json::object()));
        bool owned = active.threadId == req.threadId && (active.runId == req.runId || field(event,"resultRunId") == req.runId);
        if (!owned) {
            std::lock_guard lock(db.mutex);
            auto row = db.query("SELECT 1 FROM runs WHERE task_id=? AND environment_id=? AND thread_id=? AND run_id=?",
                                {task["id"],req.environmentId,req.threadId,req.runId});
            owned = row.next();
        }
        if (!owned || req.threadId.empty() || req.runId.empty()) throw Conflict("History reference does not belong to this task");
        const auto excerpt = t3.read(req);
        if ((!excerpt.threadId.empty() && excerpt.threadId != req.threadId) ||
            (!excerpt.runId.empty() && excerpt.runId != req.runId)) throw Conflict("History source does not match the requested Run");
        return change(task,[&](Json& current) {
            auto evidence = event["evidence"];
            const auto text = bounded(excerpt.text,8192);
            evidence.push_back({{"environmentId",req.environmentId},{"threadId",req.threadId},{"runId",req.runId},
                {"itemId",excerpt.itemId},{"text",text},{"more",excerpt.more || text.size() < excerpt.text.size()},
                {"nextCursor",excerpt.nextCursor},{"nextOffset",excerpt.nextOffset}});
            db.exec("UPDATE events SET evidence=? WHERE id=?", {evidence,event["id"]});
            db.journal(current["id"],"session_read",evidence.back());
            return true;
        });
    }

    void accept(const Json& snapshot, const Json& event, const Decision& decision) {
        const auto& args = decision.args;
        const auto& tool = decision.tool;
        if (tool == "session_read") {
            readEvidence(snapshot,event,args);
            return;
        }
        const bool work = tool == "delegate" || tool == "follow_up";
        const bool initial = field(snapshot,"doneWhen").empty();
        const auto instruction = field(args,"instruction");
        const auto doneWhen = field(args,"doneWhen");
        if (work && !present(instruction)) throw Conflict("A delegate/follow-up instruction is required");
        if (tool == "delegate" && initial && !present(doneWhen)) throw Conflict("Initial delegate requires doneWhen");
        if ((!initial || tool != "delegate") && args.contains("doneWhen")) throw Conflict("doneWhen is already fixed or unavailable");
        if (tool == "ask_user" && !present(field(args,"question"))) throw Conflict("A user question is required");
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
                                          {"decisionReason",decision.reason}});
                return true;
            }
            db.consumeEvent(current);
            const auto action = actionId();
            db.journal(id,"decision",{{"actionId",action},{"tool",tool},{"args",args},{"reason",decision.reason},
                                      {"eventId",event["id"]},{"evidence",event["evidence"]}});
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
            if (ingest(task,observed) || observed.kind == Observation::Kind::Unavailable) return;
        }
        Json event, history = Json::array();
        {
            std::lock_guard lock(db.mutex);
            event = db.currentEvent(task);
            if (event.is_null() || event["retryAt"].get<Seconds>() > options.clock()) return;
            auto rows = db.query("SELECT id,kind,data FROM journal WHERE task_id=? AND kind IN"
                " ('decision','report','user_answer','session_answered') ORDER BY id DESC LIMIT 6",{task["id"]});
            std::size_t remaining = 16000;
            while (rows.next() && remaining > 100) {
                const auto data = rows.string(2);
                const auto excerpt = bounded(data,std::min<std::size_t>(remaining,5000));
                if (data.size() == excerpt.size()) history.push_back({{"id",rows.number(0)},{"kind",rows.string(1)},{"data",Json::parse(data)}});
                else history.push_back({{"id",rows.number(0)},{"kind",rows.string(1)},{"excerpt",excerpt},{"truncated",true}});
                remaining -= excerpt.size();
            }
            std::reverse(history.begin(),history.end());
        }
        const auto tools = available(task,event);
        try {
            auto bot = notes(field(task,"botId"));
            bot.soul = bounded(bot.soul,8192);
            bot.memory = bounded(bot.memory,8192);
            Json summary = {{"id",task["id"]},{"botId",task["botId"]},{"request",task["request"]},{"doneWhen",task["doneWhen"]},
                            {"remaining",task["remaining"]},{"additionalUsed",task["additionalUsed"]}};
            const auto evidence = event["evidence"];
            event.erase("evidence");
            event.erase("consumed");
            const Decision decision = model.decide({summary,event,history,bot,tools,field(task,"feedback"),evidence,task["config"]});
            event["evidence"] = evidence;
            if (!task["activeRun"].is_null()) {
                const auto observed = observe(task);
                if (ingest(task,observed) || observed.kind == Observation::Kind::Unavailable) return;
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
