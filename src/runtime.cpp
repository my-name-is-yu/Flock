#include "storage.hpp"
#include "flock/bot_profile.hpp"
#include "flock/participation.hpp"
#include "specialist_evidence.hpp"

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
constexpr Id specialistBudgetAllowance = 12;

bool present(std::string_view value, std::size_t limit = 8000) {
    return value.size() <= limit && value.find_first_not_of(" \r\n\t") != std::string_view::npos;
}
bool hasRun(const RunRef& r) { return !r.threadId.empty() && !r.runId.empty(); }

bool commitSha(std::string_view sha) {
    return (sha.size() == 40 || sha.size() == 64) && sha.find_first_not_of("0123456789abcdef") == sha.npos;
}

Json specialistWorkspace(std::string_view sha) {
    return {{"type","worktree"},{"baseRef",sha},{"startFromOrigin",false},{"expectedCommit",sha}};
}

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

void Store::Impl::initializeSpecialistSchema() {
    exec(R"SQL(CREATE TABLE IF NOT EXISTS specialist_budgets(
 channel_id INTEGER PRIMARY KEY REFERENCES return_channels(id),
 allowance INTEGER NOT NULL CHECK(allowance>0),
 used INTEGER NOT NULL DEFAULT 0 CHECK(used>=0 AND used<=allowance)))SQL");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS specialist_delegations(
 id TEXT PRIMARY KEY REFERENCES peer_exchanges(id),
 channel_id INTEGER NOT NULL REFERENCES specialist_budgets(channel_id),
 generation INTEGER NOT NULL,
 purpose TEXT NOT NULL,
 done_when TEXT NOT NULL,
 commit_sha TEXT NOT NULL,
 parent_id TEXT UNIQUE REFERENCES specialist_delegations(id),
 sender_task INTEGER NOT NULL UNIQUE REFERENCES tasks(id),
 recipient_task INTEGER NOT NULL UNIQUE REFERENCES tasks(id)))SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS specialist_launches(
 action_id TEXT PRIMARY KEY REFERENCES outbox(id),
 delegation_id TEXT NOT NULL REFERENCES specialist_delegations(id),
 task_id INTEGER NOT NULL UNIQUE REFERENCES tasks(id))
)SQL");
}

struct Runtime::Impl {
    Store::Impl& db;
    Access access;
    Model& model;
    T3& t3;
    SoulLoader soul;
    RuntimeOptions options;
    std::future<void> channelWorker;
    std::stop_source shutdown;
    int lockFd = -1;
    std::map<Id,std::future<void>> workers;
    // Keep provider outcomes until the Task decision and attempt usage are committed together.
    std::map<Id,TaskInference> taskInferences;
    struct HistoryUnavailable : ModelError { using ModelError::ModelError; };
    std::map<std::string,std::future<void>> conversationWorkers;

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
        db.runTurn(id,model,t3,soul,options.clock,*options.botTools,options.gitHost.get(),shutdown.get_token());
    }

    Impl(Store& store, Model& m, T3& adapter, SoulLoader loader, RuntimeOptions opts)
        : db(*store.p_), access(store.access()), model(m), t3(adapter),
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
    }
    ~Impl() {
        shutdown.request_stop();
        conversationWorkers.clear();
        workers.clear();
        if (channelWorker.valid()) channelWorker.wait();
        try { persistTaskInferences(); }
        catch (const std::exception& error) {
            std::fprintf(stderr,"Flock Task inference shutdown: %s\n",error.what());
        }
        try { db.stopWorkspace(); }
        catch (const std::exception& error) {
            std::fprintf(stderr,"Flock workspace shutdown: %s\n",error.what());
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

    std::optional<std::string> observeSpecialistInput(const Person& actor, Id channel) const {
        if (!options.observeSpecialistInput) return {};
        try { return options.observeSpecialistInput(actor,channel); }
        catch (const std::exception&) { return {}; }
    }

    std::optional<Person> specialistPerson(std::string_view id) const {
        if (!options.resolveSpecialistPerson) return Person{std::string(id)};
        auto person = options.resolveSpecialistPerson(id);
        if (!person || person->id != id) return {};
        return person;
    }

    std::optional<std::string> observeSpecialistInput(std::string_view personId, Id channel) const {
        const auto person = specialistPerson(personId);
        return person ? observeSpecialistInput(*person,channel) : std::nullopt;
    }

    bool reserveSpecialistBudget(Id channel) {
        db.exec("UPDATE specialist_budgets SET used=used+1 WHERE channel_id=? AND used<allowance",{channel});
        return sqlite3_changes(db.db)!=0;
    }

    bool activeSpecialistCoordination(Id channel, Id generation) const {
        return db.query("SELECT 1 FROM session_controls WHERE channel_id=? AND generation=? AND state='active'",
                        {channel,generation}).next();
    }

    struct SpecialistOrigin {
        Id channel, generation, senderTask;
        std::string delegation, person, requester, commit, purpose, doneWhen, requestId, parent, rootBot;
    };

    std::optional<SpecialistOrigin> specialistOrigin(Id taskId) const {
        auto row = db.query(R"SQL(
SELECT d.channel_id,d.generation,d.sender_task,d.id,c.person_id,t.bot_id,d.commit_sha,d.purpose,d.done_when,
       c.request_id,d.parent_id,c.bot_id
FROM specialist_delegations d JOIN return_channels c ON c.id=d.channel_id JOIN tasks t ON t.id=d.sender_task
WHERE d.recipient_task=?1 OR d.id=(
 SELECT w.delegation_id FROM workflow_stages s JOIN workflows w ON w.id=s.workflow_id WHERE s.task_id=?1)
)SQL",{taskId});
        if (!row.next()) return {};
        return SpecialistOrigin{row.number(0),row.number(1),row.number(2),row.string(3),row.string(4),
                                row.string(5),row.string(6),row.string(7),row.string(8),row.string(9),
                                row.string(10),row.string(11)};
    }

    bool specialistAdmission(const SpecialistOrigin& origin, const Command& command, const Json& task) {
        if (!activeSpecialistCoordination(origin.channel,origin.generation) ||
            !task.at("activeRun").is_null() || !task.at("lastRun").is_null() || hasPending(task) ||
            taskIntent(task) != origin.purpose || field(task,"doneWhen") != origin.doneWhen ||
            field(task,"inputArtifactVersion") != origin.commit) return false;
        try {
            const auto person = specialistPerson(origin.person);
            if (!person) return false;
            const auto participation = db.authorizeParticipation(*person,origin.channel);
            const auto* grant = std::get_if<CoordinationActivation>(&participation);
            if (!grant || !grant->binding.request.allowDelegation) return false;
            const auto& target = grant->channel.origin.invitation.target;
            const auto peer = allowedPeer(origin.requester,command.ownerBotId);
            if (!origin.parent.empty()) {
                const auto rootPeer = allowedPeer(origin.rootBot,command.ownerBotId);
                if (!validBotRole(rootPeer.role) || rootPeer.intake.environmentId != target.environmentId ||
                    rootPeer.intake.projectId != target.projectId) return false;
                const auto parent = allowedPeer(origin.rootBot,origin.requester);
                if (!validBotRole(parent.role) || parent.intake.environmentId!=target.environmentId ||
                    parent.intake.projectId!=target.projectId ||
                    !db.query("SELECT 1 FROM specialist_delegations d JOIN tasks t ON t.id=d.recipient_task"
                              " WHERE d.id=? AND d.channel_id=? AND d.generation=? AND d.parent_id IS NULL"
                              " AND t.bot_id=? AND t.state='specialist_waiting' AND json_extract(t.data,'$.nestedDelegationId')=?",
                              {origin.parent,origin.channel,origin.generation,origin.requester,origin.delegation}).next())
                    return false;
            }
            if (!validBotRole(peer.role) || peer.intake.environmentId != target.environmentId ||
                peer.intake.projectId != target.projectId || command.environmentId != target.environmentId ||
                command.projectId != target.projectId) return false;
            const auto session = command.config.at("session");
            if (session.at("workspaceStrategy") != specialistWorkspace(origin.commit) ||
                field(session,"runtimeMode") != "approval-required") return false;
        } catch (const std::exception&) { return false; }
        return true;
    }

    bool reserveSpecialistLaunch(const SpecialistOrigin& origin, const Command& command, const Json& task,
                                 bool pinVerified) {
        if (!pinVerified || command.tool!="delegate" || !specialistAdmission(origin,command,task)) return false;
        auto existing = db.query("SELECT action_id FROM specialist_launches WHERE task_id=?",{task.at("id")});
        if (existing.next()) return existing.string(0)==command.actionId;
        if (!reserveSpecialistBudget(origin.channel)) return false;
        db.exec("INSERT INTO specialist_launches(action_id,delegation_id,task_id) VALUES(?,?,?)",
                {command.actionId,origin.delegation,task.at("id")});
        return true;
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
SELECT id FROM project_slots WHERE blocker_id IS NULL
UNION
SELECT id FROM tasks WHERE state='workflow_waiting'
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
        if (slots == 0) return {};
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
            auto turn = db.query("SELECT 1 FROM project_slots WHERE id=? AND blocker_id IS NULL"
                                 " UNION ALL SELECT 1 FROM peer_exchanges WHERE recipient_task=? AND kind='consult'",
                                 {current["id"],current["id"]});
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
        task.erase("pendingEnvironmentId");
        task.erase("environmentReleased");
        if (hasPending(task)) task["cancelledInstructions"] = task.at("pendingInstructions");
        else task.erase("cancelledInstructions");
        task.erase("pendingInstructions");
        task["activeRun"] = nullptr;
        task["state"] = "cancelled";
        if (task.contains("workflowParentTaskId"))
            db.exec("UPDATE workflow_stages SET status='stopped' WHERE task_id=?",{task.at("id")});
        db.journal(task["id"],"cancelled",Json::object());
        if (field(task,"specialistRole")=="requester") {
            auto delegation = db.query("SELECT id,commit_sha FROM specialist_delegations WHERE sender_task=?",{task.at("id")});
            if (delegation.next() && !db.query("SELECT 1 FROM journal WHERE task_id=? AND kind='specialist_result' LIMIT 1",
                                                {task.at("id")}).next()) {
                db.journal(task["id"],"specialist_result",{{"delegationId",delegation.string(0)},
                    {"artifactVersion",delegation.string(1)},
                    {"status","cancelled"},{"report","The specialist request was cancelled."},
                    {"references",Json::array()}});
            }
        }
        if (const auto peer = db.peerExchange(task["id"]); peer && peer->recipient == task["id"])
            finishPeer(*peer,task,db.load(peer->sender),"cancelled","The receiving Bot's task was cancelled.",actionId());
    }

    bool admittedSpecialistRun(const Json& task) const {
        if (!task.at("activeRun").is_null()) return true;
        const auto action = field(task,"actionId");
        return !action.empty() && db.query("SELECT 1 FROM outbox WHERE id=? AND phase='started'",{action}).next();
    }

    bool admittedSpecialistDescendant(const Json& task, std::set<Id>& visited, std::size_t& inspected) const {
        const auto taskId = task.at("id").get<Id>();
        if (inspected++ >= 128 || !visited.insert(taskId).second) return true;
        if (admittedSpecialistRun(task)) return true;
        if (const auto delegation = field(task,"nestedDelegationId"); !delegation.empty()) {
            auto child = db.query(R"SQL(
SELECT child.recipient_task FROM specialist_delegations parent
JOIN specialist_delegations child ON child.parent_id=parent.id
WHERE parent.recipient_task=? AND child.id=?
)SQL",{taskId,delegation});
            if (!child.next()) return true;
            if (admittedSpecialistDescendant(db.load(child.number(0)),visited,inspected)) return true;
        }
        auto stages = db.query(R"SQL(
SELECT s.task_id FROM specialist_delegations d JOIN workflows w ON w.delegation_id=d.id
JOIN workflow_stages s ON s.workflow_id=w.id WHERE d.recipient_task=? AND s.task_id IS NOT NULL
)SQL",{taskId});
        while (stages.next())
            if (admittedSpecialistDescendant(db.load(stages.number(0)),visited,inspected)) return true;
        return false;
    }

    bool admittedSpecialistDescendant(const Json& task) const {
        std::set<Id> visited;
        std::size_t inspected = 0;
        return admittedSpecialistDescendant(task,visited,inspected);
    }

    Id specialistBudgetRemaining(Id channel) const {
        std::lock_guard lock(db.mutex);
        auto row = db.query("SELECT allowance-used FROM specialist_budgets WHERE channel_id=?",{channel});
        return row.next() ? row.number(0) : specialistBudgetAllowance;
    }

    void freezeSpecialistWork(const Person& person, Id channel, Id generation) {
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        const auto participation = db.authorizeParticipation(person,channel,false);
        if (!std::holds_alternative<CoordinationActivation>(participation)) return;
        auto control = db.query("SELECT generation,state FROM session_controls WHERE channel_id=?",{channel});
        if (!control.next() || control.number(0)<=generation || control.string(1)=="active") return;
        auto rows = db.query(R"SQL(
SELECT recipient_task FROM specialist_delegations WHERE channel_id=? AND generation<=?
UNION SELECT s.task_id FROM specialist_delegations d JOIN workflows w ON w.delegation_id=d.id
JOIN workflow_stages s ON s.workflow_id=w.id WHERE d.channel_id=? AND d.generation<=? AND s.task_id IS NOT NULL
)SQL",{channel,generation,channel,generation});
        std::vector<Id> tasks;
        while (rows.next()) if (rows.number(0)>0) tasks.push_back(rows.number(0));
        auto emptyStages = db.query(R"SQL(
SELECT s.workflow_id,s.stage_id FROM specialist_delegations d JOIN workflows w ON w.delegation_id=d.id
JOIN workflow_stages s ON s.workflow_id=w.id WHERE d.channel_id=? AND d.generation<=? AND s.task_id IS NULL
AND s.status NOT IN ('completed','failed','stopped')
)SQL",{channel,generation});
        while (emptyStages.next())
            db.exec("UPDATE workflow_stages SET status='stopped' WHERE workflow_id=? AND stage_id=? AND task_id IS NULL",
                    {emptyStages.number(0),emptyStages.string(1)});
        for (const auto id : tasks) {
            auto task = db.load(id);
            if (terminal(task) || task["state"]=="cancelling" || admittedSpecialistRun(task) ||
                admittedSpecialistDescendant(task)) continue;
            db.requestCancellation(task);
        }
        transaction.commit();
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

    void updateSpecialistVerdict(Json& task, const Json& report) {
        if (field(task,"specialistRole")!="executor") return;
        task.erase("specialistVerdict");
        if (field(report,"status")!="completed" || field(report,"itemId").empty() ||
            report.value("truncated",false) || field(report,"evidenceStatus")=="report_unavailable") return;
        const auto check = Json::parse(field(report,"report"),nullptr,false);
        const auto verdict = field(check,"verdict");
        if (field(check,"artifactVersion")==field(task,"inputArtifactVersion") &&
            (verdict=="pass" || verdict=="fail") && specialistFindings(check))
            task["specialistVerdict"] = verdict;
    }

    void writeRunReport(Json& task, const Json& data) {
        updateSpecialistVerdict(task,data);
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

    void setReportEvidence(Json& data, const RunEvidence& evidence) {
        using Availability = RunEvidence::Availability;
        const bool missing = evidence.availability == Availability::ReportUnavailable;
        const auto report = missing ? std::string{} : bounded(evidence.report,8192);
        data["report"] = report;
        data["truncated"] = !missing && (evidence.truncated || report.size() < evidence.report.size());
        data["detail"] = bounded(evidence.detail,2048);
        data["itemId"] = missing ? std::string{} : evidence.itemId;
        data.erase("evidenceStatus");
        if (missing) data["evidenceStatus"] = "report_unavailable";
        if (evidence.availability == Availability::DiagnosticUnavailable) data["evidenceStatus"] = "diagnostic_unavailable";
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
                Json data = {{"kind","result"},{"run",task["lastRun"]},
                    {"resultRunId",observed.resultRunId.empty() ? run.runId : observed.resultRunId},
                    {"status",observed.status},{"environmentId",task["environmentId"]},
                    {"revisionId",task.at("lastRun").at("revisionId")}};
                setReportEvidence(data,{observed.evidence,observed.report,observed.itemId,observed.detail,observed.truncated});
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
                db.wait(task,"deadline",{{"text","The assigned Run reached its deadline"}});
                return true;
            }
            return false;
        }();
        if (changed) saveTask(task,std::move(resumedDecision));
        tx.commit();
        return changed;
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

    void interruptRun(const Json& task) {
        const auto& stop = task.at("runStop");
        if (task["activeRun"].is_null() || task["activeRun"] != stop.at("run") ||
            (task["state"] == "cancelling") != (stop["intent"] == "cancel")) return;
        const auto observed = observe(task);
        if (ingest(task,observed)) return;
        if (observed.kind == Observation::Kind::Finished)
            return stopNote(task,"Waiting for the assigned Run's delegated work to settle");
        if (observed.kind == Observation::Kind::Unavailable &&
            !(task.contains("workflowId") && stop.at("intent") == "cancel"))
            return stopNote(task,"Cannot locate or observe the assigned Run");
        const auto now = options.clock();
        if (now < stop.value("retryAt",Seconds{0})) return;
        Json requested;
        if (!change(task,[&](Json& current) {
            if (shutdown.stop_requested()) return false;
            current["runStop"]["retryAt"] = now + 5;
            return true;
        },&requested)) return;
        InterruptOutcome outcome;
        try { outcome = t3.interrupt(field(task,"environmentId"),task["config"],runRef(stop.at("run"))); }
        catch (const std::exception&) { return stopNote(requested,"T3 did not confirm the interrupt request"); }
        if (outcome != InterruptOutcome::Requested)
            return stopNote(requested,"Waiting for T3 to confirm the assigned Run has settled");
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        db.journal(task["id"],"interrupt_requested",stop);
        transaction.commit();
    }

    void finishReplan(const Json& task) {
        change(task,[&](Json& current) {
            const auto old = field(current,"environmentId");
            auto target = field(current,"pendingEnvironmentId");
            if (target.empty()) target = old;
            const auto id = current["id"].get<Id>();
            const auto peer = db.peerExchange(id);
            if (peer && !peer->allowsReplan(id)) return false;
            const bool released = current.value("environmentReleased",false);
            const auto event = db.currentEvent(current);
            const bool preservePeerAnswer = peer && !event.is_null() && event["consumed"] != true &&
                event["kind"] == "bot_answer";
            if (target != old && !released) {
                current["environmentReleased"] = true;
                return true;
            }
            if (target != old || released || current["state"] == "queued") {
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
            current["state"] = current.contains("workflowId") && current["lastRun"].is_null() ? "queued" : "deciding";
            for (const auto* key : {"replanning","replanningDetail","pendingEnvironmentId","environmentReleased","runStop","feedback"})
                current.erase(key);
            return true;
        });
    }

    void stop(Json task) {
        const bool cancel = task["state"] == "cancelling";
        if (cancel && (requestNestedSpecialistStop(task) || requestSpecialistWorkflowStop(task))) return;
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

    bool scopedSpecialistLaunch(const Json& task, std::string_view tool) const {
        const auto role = field(task,"specialistRole");
        return (role=="workflow_stage" || role=="executor") && (tool=="delegate" || tool=="follow_up");
    }

    void failSpecialistLaunch(Json& task, const Json& action, std::string_view detail) {
        const auto id = task.at("id").get<Id>();
        db.exec("UPDATE outbox SET phase='closed' WHERE id=? AND phase IN ('ready','started')",{action.at("id")});
        db.clearWait(task,"delivery_refused");
        task["actionId"] = "";
        task["state"] = "completed";
        task["peerOutcome"] = "failed";
        db.journal(id,"specialist_launch_blocked",{{"text",detail},{"actionId",action.at("id")}});
        if (field(task,"specialistRole")=="workflow_stage")
            db.exec("UPDATE workflow_stages SET status='failed' WHERE task_id=?",{id});
        if (const auto peer = db.peerExchange(id); peer && peer->recipient==id)
            finishPeer(*peer,task,db.load(peer->sender),"failed",detail,actionId());
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

            if (!cancelling && outcome.kind==Delivery::Kind::NotAccepted && outcome.definitelyUnsent &&
                scopedSpecialistLaunch(task,field(action,"tool"))) {
                failSpecialistLaunch(task,action,bounded(outcome.detail,2048));
                return true;
            }
            if (delivery == DeliveryOutcome::Accepted) {
                db.exec("UPDATE outbox SET phase='accepted' WHERE id=?", {action["id"]});
                db.clearWait(task,"delivery_confirmed");
                if (reply) db.unlockQuestion(task,"answered");
                else {
                    db.exec("INSERT OR IGNORE INTO runs VALUES(?,?,?,?,?,?)", {action["id"],id,
                        task["environmentId"],outcome.run.threadId,outcome.run.runId,outcome.run.link});
                    if (task.contains("workflowId")) {
                        const auto now = options.clock();
                        db.exec("UPDATE workflow_stages SET confirmed_state='running',confirmed_at=?,unreachable=0,"
                                "status=CASE WHEN status='pending' THEN 'running' ELSE status END,"
                                "deadline=CASE WHEN deadline=0 AND json_extract(spec,'$.softDeadlineSeconds')>0 "
                                "THEN ?+json_extract(spec,'$.softDeadlineSeconds') ELSE deadline END WHERE task_id=?", {now,now,id});
                    }
                    task["activeRun"] = runJson(outcome.run);
                    task["activeRun"]["ownerBotId"] = task["botId"];
                    task["activeRun"]["environmentId"] = task["environmentId"];
                    task["activeRun"]["revisionId"] = action.at("revisionId");
                    db.journal(id,"run_started",{{"run",task["activeRun"]},{"actionId",action["id"]},
                        {"revisionId",task["activeRun"]["revisionId"]},
                        {"sourceResultRefs",action.at("sourceResultRefs")}});
                    instructionDelivery(task,action);
                    if (action["tool"] == "delegate" && !task.value("replanning",false)) {
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

    std::optional<Delivery> replaySpecialistLaunch(Id id, const Command& command) {
        if (command.tool != "delegate" || command.authGeneration.empty() || !command.candidate.threadId.empty()) return {};
        const auto origin = [&]() -> std::optional<SpecialistOrigin> {
            std::lock_guard lock(db.mutex);
            if (!db.query("SELECT 1 FROM specialist_launches WHERE task_id=? AND action_id=?",
                          {id,command.actionId}).next()) return {};
            return specialistOrigin(id);
        }();
        if (!origin || t3.authGeneration(command.environmentId,command.config) != command.authGeneration ||
            observeSpecialistInput(origin->person,origin->channel) != origin->commit) return {};
        {
            std::lock_guard lock(db.mutex);
            Transaction transaction(db.db);
            const auto current = db.load(id);
            if (shutdown.stop_requested() || current.at("state") == "cancelling" || terminal(current) ||
                current.at("actionId") != command.actionId ||
                !specialistAdmission(*origin,command,current)) return {};
            transaction.commit();
        }
        // The original admission is already durable. Replay uses the same OAuth namespace and keys.
        auto replay = t3.send(command);
        if (replay.kind==Delivery::Kind::Accepted ||
            (replay.kind==Delivery::Kind::NotAccepted && replay.definitelyUnsent)) return replay;
        return {};
    }

    void deliver(const Json& task, Json action) {
        if (shutdown.stop_requested()) return;
        auto cmd = command(task,action);
        const auto id = task["id"].get<Id>();
        Delivery outcome;
        // A scoped launch must pass the live coordination check when reserving its slot.
        const bool specialistLaunch = scopedSpecialistLaunch(task,cmd.tool);
        const auto launchAllowed = [&] {
            return specialistLaunch || (cmd.tool != "delegate" && cmd.tool != "follow_up") ||
                   access.permits(cmd.ownerBotId,{"environment",cmd.environmentId,"launch"},true);
        };
        const Delivery denied{Delivery::Kind::Settings,{},"Approve launch access in Access Settings (/settings/" +
            cmd.ownerBotId + "/environment) or use 'flock access grant BOT environment ENVIRONMENT_ID launch'.",true};
        const bool firstSend = action["phase"] == "ready";
        if (firstSend) {
            if (!launchAllowed()) return applyDelivery(id,action,denied);
            if (cmd.tool == "delegate") {
                try {
                    std::lock_guard lock(db.mutex);
                    prepareDelegation(task,action);
                    cmd.instruction = field(action,"instruction");
                } catch (const Conflict& error) {
                    return applyDelivery(id,action,{Delivery::Kind::Settings,{},error.what(),true});
                }
            }
            if (cmd.tool == "delegate" || cmd.tool == "follow_up") {
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
            std::optional<SpecialistOrigin> origin;
            if (specialistLaunch) {
                std::lock_guard lock(db.mutex);
                origin = specialistOrigin(id);
            }
            const bool pinned = origin && observeSpecialistInput(origin->person,origin->channel)==origin->commit;
            bool specialistBlocked = false;
            if (!change(task,[&](Json& current) {
                if (shutdown.stop_requested() || current["actionId"] != action["id"]) return false;
                if (specialistLaunch && (!origin || !reserveSpecialistLaunch(*origin,cmd,current,pinned))) {
                    failSpecialistLaunch(current,action,
                        "The specialist launch could not verify active coordination, remaining allowance, or the pinned input");
                    specialistBlocked = true;
                    return true;
                }
                auto payload = action;
                payload.erase("phase");
                payload.erase("authGeneration");
                db.exec("UPDATE outbox SET phase='started',auth_generation=?,data=?"
                        " WHERE id=? AND phase='ready'",{cmd.authGeneration,payload,action["id"]});
                if (!sqlite3_changes(db.db)) return false;
                if (cmd.tool != "answer_session_question" && current["deadline"] == 0)
                    current["deadline"] = options.clock() + db.environmentTimeout(cmd.ownerBotId,cmd.environmentId,current.at("config"));
                db.journal(current["id"],"send_started",{{"actionId",action["id"]}});
                return true;
            }) || specialistBlocked) return;
        } else if (action["phase"] != "started") return;
        try {
            if (firstSend) outcome = launchAllowed() ? t3.send(cmd) : denied;
            else {
                outcome = t3.reconcile(cmd);
                if (outcome.kind == Delivery::Kind::Unknown) {
                    if (auto replay = replaySpecialistLaunch(id,cmd)) outcome = std::move(*replay);
                }
            }
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
        const auto id = task.at("id").get<Id>();
        if (peer && peer->sender == id && peer->phase == "question") return {"reply_to_bot","ask_user"};
        const bool specialist = field(task,"specialistRole")=="executor";
        if (!task["activeRun"].is_null() && !field(task["question"],"requestId").empty())
            return {"answer_session_question",specialist ? "reply_to_bot" : "ask_user"};
        if (field(task,"specialistRole")=="workflow_stage") return {};
        const bool receiving = peer && peer->recipient == id && peer->phase == "working";
        if (specialist) {
            if (!receiving) return {};
            if (hasSettledRun(task)) return {"reply_to_bot"};
            std::vector<std::string> tools{"delegate","reply_to_bot"};
            if (options.observeSpecialistInput) tools.push_back("start_peer_workflow");
            if (canSend) tools.push_back("delegate_nested_specialist");
            return tools;
        }
        if (receiving && peer->kind == "consult") return {"reply_to_bot"};
        std::vector<std::string> tools{"delegate",receiving ? "reply_to_bot" : "ask_user"};
        if (hasSettledRun(task)) {
            if (!task.value("newSessionRequired",false)) tools.push_back("follow_up");
            if (event.contains("sessionReadViews")) tools.push_back("session_read");
        }
        if (!receiving) {
            std::lock_guard lock(db.mutex);
            if (canComplete(task,event)) tools.push_back("complete");
        }
        if (canRead && event["evidence"].size() < maxEvidenceReads) tools.push_back("read_bot_evidence");
        if (canSend) tools.push_back("send_bot_message");
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
        if (!access.permits(field(task,"botId"),Scope::sessionRead(req.environmentId,req.threadId),true))
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
       (j.kind='user_answer' AND j.id>?3 AND w.resolution='answer') OR
       (j.kind='instruction_steered' AND json_extract(j.data,'$.revisionId')=?2))
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
            instruction += answer ? "\n\nUser answer" : kind == "instruction_steered" ? "\n\nDelivered Session steer"
                                                                                       : "\n\nAccepted instruction";
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
        if (field(task,"specialistRole")=="executor") {
            instruction += "\n\nCheck only this immutable commit in the assigned worktree: " +
                field(task,"inputArtifactVersion") +
                ". Verify HEAD before checking. Keep the worktree unchanged. Return only a JSON object with "
                "artifactVersion, verdict (pass or fail), and findings (one nonempty JSON string).";
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
        action["instruction"] = std::move(instruction);
        action["instructions"] = scrub.record(std::move(prompt.pending),SecretRedactor::Fields::external);
        action["contextIds"] = std::move(prompt.contextIds);
        action["sourceResultRefs"] = std::move(prompt.resultReferences);
    }

    void queueAction(Json& task, const std::string& action, std::string_view tool, std::string instruction,
                     const Json& args, const Json& event, bool charge) {
        WorkPrompt prompt;
        if (tool == "follow_up") {
            prompt = workPrompt(task,event,action.size());
            const auto scrub = db.redactor(field(task,"botId"));
            prompt.pending = scrub.record(std::move(prompt.pending),SecretRedactor::Fields::external);
            instruction += prompt.instruction;
            instruction = sessionPrompt(action,scrub.text(instruction));
        }
        if (tool == "delegate" || tool == "follow_up") task["deadline"] = 0;
        Json out = {{"id",action},{"tool",tool},{"environmentId",task["environmentId"]},
            {"target",tool == "follow_up" ? task["lastRun"] : task["activeRun"]},
            {"instruction",std::move(instruction)},{"questionId",field(args,"requestId")},
            {"answers",args.value("answers",Json::object())},{"revisionId",revision(task)},
            {"instructions",prompt.pending},{"contextIds",prompt.contextIds},{"sourceResultRefs",prompt.resultReferences},{"chargesBudget",charge}};
        if (tool == "delegate") {
            out["instructionBase"] = db.redactor(field(task,"botId")).text(field(out,"instruction"));
            out.erase("instruction");
        }
        db.exec("INSERT INTO outbox(id,task_id,phase,data) VALUES(?,?,'ready',?)", {action,task["id"],out});
        task["actionId"] = action;
        task["state"] = "sending";
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

    Id openPeerExchange(Json& sender, const Intake& intake, const std::string& kind,
                        std::string_view note, const std::string& action, bool charge,
                        Json forwarded = {}, const SpecialistCheck* specialist = nullptr, Id queueOwner = 0) {
        const auto id = db.submit(intake,0,nullptr,RequestJournal::skip);
        auto recipient = db.load(id);
        const PeerExchange exchange{action,kind,"working","",sender["id"],id};
        auto message = peerMessage(exchange,sender,recipient,"request",note,action);
        message["chargesBudget"] = charge;
        if (!forwarded.is_null()) message["forwardedMessage"] = std::move(forwarded);
        if (specialist) {
            recipient["specialistRole"] = "executor";
            recipient["specialistDelegationId"] = specialist->delegationId;
            recipient["inputArtifactVersion"] = specialist->commitSha;
            if (queueOwner) recipient["specialistQueueOwner"] = queueOwner;
            db.addRevision(recipient,specialist->purpose,specialist->doneWhen);
            message["inputArtifactVersion"] = specialist->commitSha;
            message["doneWhen"] = specialist->doneWhen;
        }
        transitionPeer(exchange,sender,recipient,std::move(message));
        return id;
    }

    SpecialistDelegation createSpecialist(const SpecialistCheck& check, std::string_view requester,
                                          PeerBot peer, const SpecialistOrigin* parent = nullptr) {
        peer.intake.request = check.purpose;
        auto& session = peer.intake.config["session"];
        session["workspaceStrategy"] = specialistWorkspace(check.commitSha);
        session["runtimeMode"] = "approval-required";
        session["interactionMode"] = "default";
        if (!reserveSpecialistBudget(check.channel))
            throw SpecialistAllowanceExhausted("The coordination work allowance is exhausted");
        const Id senderId = db.submit({std::string(requester),check.purpose,peer.intake.environmentId,peer.intake.projectId},
                                      0,nullptr,RequestJournal::skip);
        auto sender = db.load(senderId);
        sender["specialistRole"] = "requester";
        sender["specialistDelegationId"] = check.delegationId;
        if (parent) sender["specialistQueueOwner"] = parent->senderTask;
        db.addRevision(sender,check.purpose,check.doneWhen);
        const auto recipientId = openPeerExchange(sender,peer.intake,"work",check.purpose,check.delegationId,
                                                 false,{},&check,parent ? parent->senderTask : 0);
        saveTask(sender);
        db.exec(R"SQL(
INSERT INTO specialist_delegations(id,channel_id,generation,purpose,done_when,commit_sha,parent_id,sender_task,recipient_task)
VALUES(?,?,?,?,?,?,?,?,?)
)SQL",{check.delegationId,check.channel,check.generation,check.purpose,check.doneWhen,check.commitSha,
         parent ? Json(parent->delegation) : Json(nullptr),senderId,recipientId});
        return {check.delegationId,senderId,recipientId};
    }

    SpecialistDelegation startSpecialist(const Person& person, const SpecialistCheck& command) {
        if (command.channel<=0 || command.generation<=0 || !present(command.delegationId,128) ||
            command.delegationId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:/")!=std::string::npos ||
            !validBotId(command.peerBotId) || !present(command.purpose) || !present(command.doneWhen) ||
            !commitSha(command.commitSha)) throw Conflict("Invalid specialist work binding");
        const auto observedPin = observeSpecialistInput(person,command.channel);
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        const auto participation = db.authorizeParticipation(person,command.channel);
        const auto* grant = std::get_if<CoordinationActivation>(&participation);
        if (!grant || !grant->binding.request.allowDelegation)
            throw Conflict("The Session has no specialist delegation permission");
        const auto& invitation = grant->channel.origin.invitation;
        const auto& target = invitation.target;
        const auto payload = Json::array({command.delegationId,command.peerBotId,command.purpose,command.doneWhen,command.commitSha});
        db.sharedRedactor().requirePublic(payload);
        auto existing = db.query(R"SQL(
SELECT d.channel_id,d.generation,d.purpose,d.done_when,d.commit_sha,d.sender_task,d.recipient_task,r.bot_id,d.parent_id
FROM specialist_delegations d JOIN tasks r ON r.id=d.recipient_task WHERE d.id=?
)SQL",{command.delegationId});
        if (existing.next()) {
            if (existing.number(0)!=command.channel || existing.number(1)!=command.generation ||
                existing.string(2)!=command.purpose || existing.string(3)!=command.doneWhen ||
                existing.string(4)!=command.commitSha || existing.string(7)!=command.peerBotId || !existing.string(8).empty())
                throw Conflict("This specialist delegation already has a different binding");
            const SpecialistDelegation result{command.delegationId,existing.number(5),existing.number(6)};
            transaction.commit();
            return result;
        }
        if (!activeSpecialistCoordination(command.channel,command.generation))
            throw Conflict("Session coordination is not active at the expected generation");
        if (observedPin!=command.commitSha) throw Conflict("The current immutable input could not be verified");
        auto peer = allowedPeer(invitation.botId,command.peerBotId);
        if (!validBotRole(peer.role) || peer.intake.environmentId!=target.environmentId || peer.intake.projectId!=target.projectId)
            throw Conflict("The specialist is not authorized to check this project");
        if (db.query("SELECT 1 FROM peer_exchanges WHERE id=?",{command.delegationId}).next())
            throw Conflict("The specialist delegation identifier is already in use");
        db.exec("INSERT OR IGNORE INTO specialist_budgets(channel_id,allowance) VALUES(?,?)",
                {command.channel,specialistBudgetAllowance});
        const auto delegation = createSpecialist(command,invitation.botId,std::move(peer));
        transaction.commit();
        return delegation;
    }

    bool canDelegateNested(const SpecialistOrigin& origin, const Json& task) const {
        if (!origin.parent.empty() || task.contains("nestedDelegationId") ||
            !task.at("activeRun").is_null() || !task.at("lastRun").is_null() ||
            !activeSpecialistCoordination(origin.channel,origin.generation)) return false;
        try {
            const auto person = specialistPerson(origin.person);
            if (!person) return false;
            const auto participation = db.authorizeParticipation(*person,origin.channel);
            const auto* activation = std::get_if<CoordinationActivation>(&participation);
            return activation && activation->binding.request.allowDelegation;
        } catch (const Conflict&) {
            return false;
        }
    }

    bool scopedPeer(const SpecialistOrigin& origin, const Json& task, const PeerBot& peer) const {
        const auto matchesScope = [&](const PeerBot& candidate) {
            return validBotRole(candidate.role) && candidate.intake.environmentId==field(task,"environmentId") &&
                candidate.intake.projectId==field(task,"projectId");
        };
        if (peer.intake.botId==origin.rootBot || peer.intake.botId==field(task,"botId") || !matchesScope(peer))
            return false;
        try {
            return matchesScope(allowedPeer(origin.rootBot,peer.intake.botId));
        } catch (const Conflict&) {
            return false;
        }
    }

    void startNestedSpecialist(const Json& task, const Json& event, const TaskDecision& decision) {
        const auto id = task.at("id").get<Id>();
        const auto origin = [&] {
            std::lock_guard lock(db.mutex);
            return specialistOrigin(id);
        }();
        const auto peerBot = field(decision.args,"botId");
        if (!origin || decision.args.size()!=1 || !validBotId(peerBot)) throw Conflict("Choose a permitted nested specialist");
        const auto pin = observeSpecialistInput(origin->person,origin->channel);
        if (pin!=origin->commit) throw Conflict("The current immutable input could not be verified");
        change(task,[&](Json& current) {
            const auto peer = db.peerExchange(id);
            const auto cmd = command(current,{{"environmentId",current.at("environmentId")}});
            if (!peer || peer->recipient!=id || peer->id!=origin->delegation || peer->phase!="working" ||
                !canDelegateNested(*origin,current) || !specialistAdmission(*origin,cmd,current))
                throw Conflict("The specialist assignment cannot delegate further work");
            auto target = allowedPeer(cmd.ownerBotId,peerBot);
            if (!scopedPeer(*origin,current,target)) throw Conflict("The nested specialist is outside the assignment scope");
            const auto action = actionId();
            const SpecialistCheck request{origin->channel,origin->generation,action,peerBot,origin->purpose,
                                          origin->doneWhen,origin->commit};
            createSpecialist(request,cmd.ownerBotId,std::move(target),&*origin);
            recordDecision(current,event,decision,action);
            consumeEvent(current);
            current["nestedDelegationId"] = request.delegationId;
            current["state"] = "specialist_waiting";
            return true;
        });
    }

    void startSpecialistWorkflow(const Json& task, const Json& event, const TaskDecision& decision) {
        const auto id = task.at("id").get<Id>();
        const auto origin = [&] {
            std::lock_guard lock(db.mutex);
            return specialistOrigin(id);
        }();
        if (!origin || !decision.args.empty()) throw Conflict("Use the current specialist assignment for the workflow");
        const auto pin = observeSpecialistInput(origin->person,origin->channel);
        if (pin!=origin->commit) throw Conflict("The current immutable input could not be verified");
        change(task,[&](Json& current) {
            const auto peer = db.peerExchange(id);
            const auto cmd = command(current,{{"environmentId",current.at("environmentId")}});
            if (!peer || peer->recipient!=id || peer->id!=origin->delegation || peer->phase!="working" ||
                current.contains("delegatedWorkflowId") || !specialistAdmission(*origin,cmd,current))
                throw Conflict("The specialist assignment changed before workflow admission");
            for (int i=0;i<2;++i) {
                if (!reserveSpecialistBudget(origin->channel))
                    throw SpecialistAllowanceExhausted("The coordination work allowance is exhausted");
            }
            const auto action = actionId();
            const Json stages = Json::array({
                {{"stageId","verify-1"},{"name","Independent check 1"},{"role","verify"},{"request",origin->purpose}},
                {{"stageId","verify-2"},{"name","Independent check 2"},{"role","verify"},{"request",origin->purpose}}});
            const Json routing = {{"environmentId",current.at("environmentId")},{"projectId",current.at("projectId")},
                                  {"config",current.at("config")}};
            const auto workflow = db.startWorkflow(ReceivedPeerWork{peer->id,origin->requestId,action},
                {{"request",origin->purpose},{"stages",stages}},cmd.ownerBotId,routing,origin->commit);
            db.materializeWorkflowStages(workflow);
            auto assigned = db.query("SELECT count(*) FROM workflow_stages WHERE workflow_id=? AND task_id IS NOT NULL",{workflow});
            if (!assigned.next() || assigned.number(0)!=2) throw Conflict("Both independent checks must be assigned together");
            recordDecision(current,event,decision,action);
            consumeEvent(current);
            current["delegatedWorkflowId"] = workflow;
            current["state"] = "workflow_waiting";
            db.journal(id,"specialist_workflow_started",{{"workflowId",workflow},{"artifactVersion",origin->commit}});
            return true;
        });
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
            if (!admitWork(sender,decision,charge)) return;
            establishCriteria(sender,args);
            if (db.peerWorkRouteWouldCycle(sender.at("id"),peer.intake.environmentId,peer.intake.projectId))
                throw Conflict("This work request would make two project queues wait for one another");
        }
        peer.intake.request = note.empty() ? field(forwarded,"text") : note;
        openPeerExchange(sender,peer.intake,kind,note,action,charge,std::move(forwarded));
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
        if (!access.permits(field(task,"botId"),Scope::sessionRead(env,thread),true))
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
                updateSpecialistVerdict(current,report);
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
        if (field(recipient,"specialistRole")=="executor") {
            message["inputArtifactVersion"] = recipient.at("inputArtifactVersion");
            if (recipient.contains("specialistVerdict")) message["verdict"] = recipient.at("specialistVerdict");
            if (recipient.contains("delegatedWorkflowId")) {
                message["workflowId"] = recipient.at("delegatedWorkflowId");
                message["workflowStatus"] = field(recipient,"delegatedWorkflowStatus");
                message["evidence"] = recipient.value("delegatedWorkflowEvidence",Json::array());
            }
            if (const auto nested = recipient.find("nestedResult"); nested != recipient.end()) {
                message["sourceExecutorBotId"] = nested->at("fromBotId");
                message["sourceDelegationId"] = nested->at("exchangeId");
                message["evidence"] = nested->value("references",Json::array());
                if (nested->contains("workflowId")) {
                    message["workflowId"] = nested->at("workflowId");
                    message["workflowStatus"] = nested->at("workflowStatus");
                }
            }
        }
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
            if (field(task,"specialistRole")=="executor") {
                if (task["activeRun"].is_null()) kinds.push_back("failed");
                if (hasSettledRun(task) && field(task,"specialistVerdict")=="pass") kinds.push_back("result");
                return kinds;
            }
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

    void settleSpecialistRequester(const Json& task) {
        change(task,[&](Json& current) {
            const auto event = db.currentEvent(current);
            const auto kind = field(event,"kind");
            if (kind=="bot_question") {
                if (event.contains("userAnswer")) {
                    const TaskDecision answer{{"reply_to_bot",{{"kind","answer"},{"text",field(event,"userAnswer")}},
                                               "Forward the person's answer to this specialist"}};
                    return replyPeer(current,event,answer,actionId());
                }
                db.wait(current,"user_question",{{"text",field(event,"text")}});
                current["wait"]["target"] = questionTarget(current);
                return true;
            }
            if (kind!="bot_result") return false;
            consumeEvent(current);
            current["state"] = "completed";
            current["peerOutcome"] = event.at("status");
            Json result = {
                {"delegationId",current.at("specialistDelegationId")},{"status",event.at("status")},
                {"executorBotId",event.at("fromBotId")},{"report",field(event,"text")},
                {"artifactVersion",field(event,"inputArtifactVersion")},{"verdict",field(event,"verdict")},
                {"references",event.value("references",Json::array())}};
            if (event.contains("workflowId")) {
                result["workflowId"] = event.at("workflowId");
                result["workflowStatus"] = event.at("workflowStatus");
            }
            if (event.contains("sourceDelegationId")) {
                result["sourceDelegationId"] = event.at("sourceDelegationId");
                result["sourceExecutorBotId"] = event.at("sourceExecutorBotId");
            }
            db.journal(current["id"],"specialist_result",result);
            settleNestedSpecialist(current,event);
            return true;
        });
    }

    void settleNestedSpecialist(const Json& child, const Json& event) {
        auto source = db.query(R"SQL(
SELECT parent.recipient_task,parent.id,parent.commit_sha FROM specialist_delegations d
 JOIN specialist_delegations parent ON parent.id=d.parent_id WHERE d.sender_task=? AND d.id=?
)SQL",{child.at("id"),event.at("exchangeId")});
        if (!source.next()) return;
        auto parent = db.load(source.number(0));
        if (parent.at("state")!="specialist_waiting" || parent.at("nestedDelegationId")!=event.at("exchangeId")) return;
        const auto peer = db.peerExchange(parent.at("id"));
        if (!peer || peer->id!=source.string(1) || peer->recipient!=parent.at("id")) return;
        const bool passed = field(event,"status")=="completed" && field(event,"verdict")=="pass" &&
            field(event,"inputArtifactVersion")==source.string(2) && !event.value("references",Json::array()).empty();
        if (passed) parent["specialistVerdict"] = "pass";
        parent["nestedResult"] = event;
        parent["peerOutcome"] = passed ? "completed" : "failed";
        parent["state"] = "completed";
        finishPeer(*peer,parent,db.load(peer->sender),field(parent,"peerOutcome"),field(event,"text"),actionId());
        saveTask(parent);
    }

    bool requestNestedSpecialistStop(const Json& task) {
        const auto delegation = field(task,"nestedDelegationId");
        if (delegation.empty()) return false;
        bool pending = false;
        change(task,[&](Json& current) {
            auto child = db.query("SELECT sender_task FROM specialist_delegations WHERE id=?",{delegation});
            if (!child.next()) return false;
            auto sender = db.load(child.number(0));
            pending = !terminal(sender);
            if (!pending || sender.at("state")=="cancelling") return false;
            db.requestCancellation(sender);
            db.journal(current.at("id"),"nested_specialist_stop_requested",{{"delegationId",delegation}});
            return true;
        });
        return pending;
    }

    bool scopedWorkflowSettled(Id workflow) const {
        return !db.query(R"SQL(
SELECT 1 FROM workflow_stages s LEFT JOIN tasks t ON t.id=s.task_id WHERE s.workflow_id=?
 AND ((s.task_id IS NULL AND s.status NOT IN ('failed','stopped')) OR
      (s.task_id IS NOT NULL AND (s.status NOT IN ('completed','failed','stopped') OR t.state NOT IN ('completed','cancelled'))))
)SQL",{workflow}).next();
    }

    bool requestSpecialistWorkflowStop(const Json& task) {
        if (!task.contains("delegatedWorkflowId")) return false;
        const auto workflow = task.at("delegatedWorkflowId").get<Id>();
        bool pending = false;
        change(task,[&](Json&) {
            pending = !scopedWorkflowSettled(workflow);
            if (!pending) return false;
            db.exec("UPDATE workflows SET stop_requested=1 WHERE id=? AND NOT stop_requested",{workflow});
            auto children = db.query("SELECT t.id FROM workflow_stages s JOIN tasks t ON t.id=s.task_id"
                                     " WHERE s.workflow_id=? AND t.state NOT IN ('cancelling','cancelled','completed')",{workflow});
            while (children.next()) {
                auto child = db.load(children.number(0));
                db.requestCancellation(child);
            }
            return true;
        });
        return pending;
    }

    void settleSpecialistWorkflow(const Json& task) {
        const auto id = task.at("id").get<Id>();
        const auto workflow = task.at("delegatedWorkflowId").get<Id>();
        const auto origin = [&]() -> std::optional<SpecialistOrigin> {
            std::lock_guard lock(db.mutex);
            if (!scopedWorkflowSettled(workflow)) return {};
            return specialistOrigin(id);
        }();
        if (!origin) return;
        const auto pin = observeSpecialistInput(origin->person,origin->channel);
        change(task,[&](Json& current) {
            if (!scopedWorkflowSettled(workflow)) return false;
            const auto peer = db.peerExchange(id);
            if (!peer || peer->id!=origin->delegation || peer->recipient!=id) return false;
            const auto checked = db.workflowStatus(workflow,field(current,"botId"),options.clock());
            auto evidence = Json::array();
            for (const auto& verdict : checked.at("verdicts")) {
                const auto& run = verdict.at("runRef");
                Json source{{"environmentId",current.at("environmentId")},{"threadId",run.at("threadId")},
                    {"runId",verdict.at("resultRunId")},{"itemId",verdict.at("itemId")},{"link",run.at("link")},
                    {"stageId",verdict.at("stageId")},{"verdict",verdict.at("verdict")},
                    {"artifactVersion",verdict.at("artifactVersion")}};
                if (verdict.contains("findings")) source["findings"] = verdict.at("findings");
                evidence.push_back(std::move(source));
            }
            const bool passed = pin==origin->commit && checked.at("status")=="completed" && evidence.size()==2;
            const auto status = !pin ? "input_unverified" : *pin!=origin->commit ? "stale" : passed ? "completed" : "failed";
            const auto outcome = passed ? "completed" : "failed";
            const auto report = passed ? "Both independent checks passed for the same immutable input" :
                !pin ? "The current immutable input could not be verified at aggregation" :
                *pin!=origin->commit ? "The root input advanced before verification settled" : "Independent verification did not pass both checks";
            if (passed) current["specialistVerdict"] = "pass";
            current["delegatedWorkflowStatus"] = status;
            current["delegatedWorkflowEvidence"] = std::move(evidence);
            current["peerOutcome"] = outcome;
            current["state"] = "completed";
            finishPeer(*peer,current,db.load(peer->sender),outcome,report,actionId());
            db.journal(id,"specialist_workflow_settled",{{"workflowId",workflow},{"status",status},{"artifactVersion",origin->commit}});
            return true;
        });
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
        if (!access.permits(field(task,"botId"),Scope::sessionRead(req.environmentId,req.threadId),true)) throw Conflict("Session read access required");
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
                !access.permits(field(task,"botId"),Scope::sessionRead(environment,thread),true)) return;
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

    void accept(const Json& snapshot, const Json& event, const TaskDecision& decision) {
        const auto& args = decision.args;
        const auto& tool = decision.tool;
        if (tool == "start_peer_workflow") return startSpecialistWorkflow(snapshot,event,decision);
        if (tool == "delegate_nested_specialist") return startNestedSpecialist(snapshot,event,decision);
        if (isGitTool(tool)) {
            Json result;
            try {
                result = db.gitTool(*options.gitHost,field(snapshot,"botId"),snapshot.at("id"),snapshot,decision,
                                    gitInvocation(decision),snapshot.at("generation").get<Id>());
            } catch (const StorageError&) { throw; }
            catch (const std::exception& error) { result = {{"ok",false},{"error",db.sharedRedactor().text(error.what())}}; }
            change(snapshot,[&](Json& current) {
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
        if (task.contains("sourceMessageId")) summary["sourceMessageId"] = task.at("sourceMessageId");
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
                 needsDoneWhen(task),summary.contains("forwardableUserMessage")},0);
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
            if (peer && peer->recipient==task.at("id") && peer->phase=="working" && options.observeSpecialistInput &&
                field(task,"specialistRole")=="executor") {
                std::lock_guard lock(db.mutex);
                const auto origin = specialistOrigin(task.at("id"));
                if (origin && canDelegateNested(*origin,task)) {
                    for (const auto& candidate : peersFor(field(task,"botId"))) {
                        if (scopedPeer(*origin,task,candidate)) choices.push_back({{"botId",candidate.intake.botId},
                            {"name",candidate.name},{"role",candidate.role}});
                    }
                }
            }
            Json pending = Json::array();
            if (hasPending(task)) {
                std::lock_guard lock(db.mutex);
                pending = workPrompt(task,event,0).pending;
            }
            auto tools = available(task,event,peer,!choices.empty(),canRead);
            if (options.gitHost && !task.contains("specialistRole") && task["activeRun"].is_null() && event["kind"] == "result")
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
        if (task.contains("workflowParentTaskId") ||
            (!task.contains("workflowId") && !task.value("freshContextRequested",false)) ||
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

    void step(Id id) {
        if (shutdown.stop_requested()) return;
        const auto task = snapshot(id);
        const auto state = field(task,"state");
        if (task.contains("pendingStageReport"))
            change(task,[&](Json& current) { return db.settleStageReport(current); });
        else if (shouldStop(task)) stop(task);
        else if (task.contains("pendingRunReport")) recoverRunReport(task);
        else if (state == "queued") activate(task);
        else if (state == "workflow_waiting") settleSpecialistWorkflow(task);
        else if (state == "deciding") {
            if (field(task,"specialistRole")=="requester") settleSpecialistRequester(task);
            else if (!continueStage(task)) decide(task);
        } else {
            if (!task["activeRun"].is_null() && (state == "running" || state == "waiting") &&
                ingest(task,observe(task))) return;
            const bool reconcile = state == "waiting" &&
                (task.contains("workflowId") || field(task,"specialistRole")=="executor") &&
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
                if (action.at("phase")!="started" || now<task.value("workflowReconcileAt",Seconds{})) return;
                if (!change(task,[&](Json& current) { current["workflowReconcileAt"] = now+5; return true; })) return;
            }
            deliver(task,action);
        }
    }
};

Runtime::Runtime(Store& store, Model& model, T3& t3, SoulLoader soul, RuntimeOptions options)
    : p_(std::make_unique<Impl>(store,model,t3,std::move(soul),std::move(options))) {}
Runtime::~Runtime() = default;
SpecialistDelegation Runtime::startSpecialist(const Person& person, const SpecialistCheck& command) {
    return p_->startSpecialist(person,command);
}
Id Runtime::specialistBudgetRemaining(Id channel) const {
    return p_->specialistBudgetRemaining(channel);
}
void Runtime::freezeSpecialistWork(const Person& person, Id channel, Id generation) {
    p_->freezeSpecialistWork(person,channel,generation);
}
void Runtime::requestStop() { p_->shutdown.request_stop(); }
void Runtime::runConversations() {
    if (p_->shutdown.stop_requested()) return;
    p_->db.persistWorkspaceResults();
    p_->db.deliverGitWebhooks();
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
    p_->persistTaskInferences();
    p_->db.persistWorkspaceResults();
    p_->fireDueAutomations();
    p_->db.deliverGitWebhooks();
    for (const auto& [bot,id] : p_->db.pendingConversations(Impl::batchLimit,p_->options.clock()))
        p_->runTurn(id);
    p_->db.advanceWorkflows(p_->options.clock());
    for (auto id : p_->nextProjects(Impl::batchLimit)) p_->step(id);
    p_->db.deliverChannels(p_->shutdown.get_token());
}
void Runtime::tickConcurrent() {
    if (p_->shutdown.stop_requested()) return;
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
        for (auto id : p_->nextProjects(Impl::batchLimit - p_->workers.size()))
            p_->workers.emplace(id,std::async(std::launch::async,[impl = p_.get(),id] { impl->step(id); }));
    });
}
} // namespace flockv0
