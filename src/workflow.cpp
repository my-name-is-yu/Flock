#include "conversation_storage.hpp"

namespace flockv0 {
namespace {
bool fullCommit(std::string_view version) {
    return (version.size()==40 || version.size()==64) &&
        version.find_first_not_of("0123456789abcdef")==std::string_view::npos;
}

bool closed(std::string_view status) {
    return status=="completed" || status=="failed" || status=="stopped";
}
}

void Store::Impl::initializeWorkflowSchema() {
    exec(R"SQL(
CREATE TABLE workflows(
    id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, request TEXT NOT NULL,
    verification_criteria TEXT NOT NULL, message_id INTEGER NOT NULL REFERENCES messages,
    soft_deadline_seconds INTEGER NOT NULL, stop_requested INTEGER NOT NULL DEFAULT 0)
)SQL");
    exec(R"SQL(
CREATE TABLE workflow_steps(
    workflow_id INTEGER NOT NULL REFERENCES workflows,
    step TEXT NOT NULL CHECK(step IN ('implement','verify')),
    task_id INTEGER NOT NULL UNIQUE REFERENCES tasks,
    result_id INTEGER REFERENCES journal,
    status TEXT NOT NULL DEFAULT 'pending',
    deadline INTEGER NOT NULL DEFAULT 0,
    confirmed_state TEXT NOT NULL DEFAULT '',
    confirmed_at INTEGER NOT NULL DEFAULT 0,
    unreachable INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(workflow_id,step))
)SQL");
    exec(R"SQL(
CREATE TABLE workflow_evidence(
    report_id INTEGER PRIMARY KEY REFERENCES journal,
    version TEXT NOT NULL,
    verdict TEXT CHECK(verdict IN ('pass','fail')))
)SQL");
}

Json Store::Impl::workflowStatus(Id id, std::string_view bot, Seconds now) const {
    auto owner = query("SELECT stop_requested,request FROM workflows WHERE id=? AND bot_id=?",{id,bot});
    if (!owner.next()) throw Conflict("Unknown workflow");
    const bool stopping = owner.number(0);
    Json result = {{"ok",true},{"workflowId",id},{"request",owner.string(1)},{"stages",Json::array()},
                   {"artifacts",Json::array()},{"verdicts",Json::array()}};
    bool finished = true, attention = false, failed = false, stopped = false;
    auto rows = query(R"SQL(
WITH steps(step,position) AS (VALUES ('implement',1),('verify',2))
SELECT steps.step,s.status,s.task_id,s.deadline,t.data,t.state,s.confirmed_state,s.confirmed_at,s.unreachable,
       pending.phase='started' AND json_extract(t.data,'$.activeRun') IS NULL,e.version,e.verdict
FROM steps
LEFT JOIN workflow_steps s ON s.workflow_id=? AND s.step=steps.step
LEFT JOIN tasks t ON t.id=s.task_id
LEFT JOIN outbox pending ON pending.id=json_extract(t.data,'$.actionId')
LEFT JOIN workflow_evidence e ON e.report_id=s.result_id
ORDER BY steps.position
)SQL",{id});
    while (rows.next()) {
        const auto step = rows.string(0);
        const Id taskId = rows.number(2);
        const auto task = taskId ? rows.json(4) : Json::object();
        const auto taskState = rows.string(5);
        auto status = taskId ? rows.string(1) : stopping || stopped ? "stopped" : "pending";
        if (taskState=="cancelled") status = "stopped";
        else if (!closed(status) && (rows.number(8) || rows.number(9))) status = "unreachable";
        else if (taskState=="cancelling") status = "stop_requested";
        const auto active = task.value("activeRun",Json(nullptr));
        const auto run = active.is_object() ? active : task.value("lastRun",Json(nullptr));
        const bool exceeded = !closed(status) && rows.number(3)>0 && now>=rows.number(3);
        const bool needs = exceeded || status=="failed" || status=="unreachable" ||
            taskState=="waiting" || task.contains("pendingRunReport");
        finished &= closed(status) && (!taskId || terminal(task)) && active.is_null() && !rows.number(9);
        attention |= needs;
        failed |= status=="failed";
        stopped |= status=="stopped";
        const bool verify = step=="verify";
        const auto version = rows.string(10);
        const auto verdict = rows.string(11);
        Json stage = {{"stageId",step},{"name",verify ? "Verify" : "Implement"},{"role",step},
            {"taskId",taskId},{"runRef",run},{"status",status},
            {"stopRequested",stopping || taskState=="cancelling" || status=="stopped"},
            {"softDeadlineAt",rows.number(3)},{"deadlineExceeded",exceeded},{"needsAttention",needs},
            {"lastConfirmedState",rows.string(6)},{"lastConfirmedAt",rows.number(7)},
            {"inputArtifactVersion",field(task,"inputArtifactVersion")},
            {"outputArtifactVersion",status=="completed" ? version : ""}};
        if (verify) stage["verified"] = status=="completed" && verdict=="pass";
        if (!verdict.empty() && (status=="completed" || status=="failed")) stage["currentVerdict"] = verdict;
        result["stages"].push_back(std::move(stage));
    }
    auto evidence = query(R"SQL(
SELECT s.step,j.task_id,e.version,e.verdict,j.id,j.data FROM workflow_steps s
JOIN journal j ON j.task_id=s.task_id JOIN workflow_evidence e ON e.report_id=j.id
WHERE s.workflow_id=? ORDER BY j.id
)SQL",{id});
    while (evidence.next()) {
        const auto report = evidence.json(5);
        Json item = {{"stageId",evidence.string(0)},{"taskId",evidence.number(1)},{"artifactVersion",evidence.string(2)},
                     {"messageId",evidence.number(4)},{"runRef",report.at("run")},{"resultRunId",report.at("resultRunId")},
                     {"itemId",report.at("itemId")}};
        const auto verdict = evidence.string(3);
        if (!verdict.empty()) item["verdict"] = verdict;
        result[verdict.empty() ? "artifacts" : "verdicts"].push_back(std::move(item));
    }
    result["status"] = stopping ? (finished ? "stopped" : "stop_requested") :
        failed ? "failed" : finished ? (stopped ? "stopped" : "completed") : "running";
    result["allStopped"] = stopping && finished;
    result["needsAttention"] = attention;
    return result;
}

Json Store::workflows(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db,Transaction::Mode::read);
    Json result = Json::array();
    auto rows = p_->query("SELECT id FROM workflows WHERE bot_id=? AND (?=0 OR id<?) ORDER BY id DESC LIMIT ?",
                          {bot,before,before,std::min(limit,50u)});
    while (rows.next()) result.push_back(p_->workflowStatus(rows.number(0),bot,unixTime()));
    tx.commit();
    return result;
}

Id Store::Impl::startWorkflow(Id message, const Json& args, std::string_view bot, const Json& routing, T3& t3) {
    for (const auto& arg : args.items())
        if (arg.key()!="request" && arg.key()!="verificationCriteria" && arg.key()!="softDeadlineSeconds")
            throw Conflict("Workflow supports implementation and independent verification. Use a T3 Session for custom stages.");
    const auto request = conversation_storage::text(args,"request",8000,true);
    const auto criteria = args.contains("verificationCriteria")
        ? conversation_storage::text(args,"verificationCriteria",8000,true)
        : "Verify the requested change.";
    const auto deadline = args.contains("softDeadlineSeconds") ? requiredInteger(args,"softDeadlineSeconds",1) : 0;
    if (deadline>31536000) throw Conflict("Invalid soft deadline");
    const auto environment = taskEnvironment(bot,routing);
    if (environment.empty()) throw Conflict("Choose a connected T3 environment for this workflow");
    t3.requireLocalWorkflow(environment);
    const auto scrub = redactor(bot);
    scrub.requirePublic({bot,routing,environment});
    const auto safeRequest = scrub.text(request);
    exec("INSERT INTO workflows(bot_id,request,verification_criteria,message_id,soft_deadline_seconds) VALUES(?,?,?,?,?)",
         {bot,safeRequest,scrub.text(criteria),message,deadline});
    const auto id = last();
    const auto taskId = startConversationTask(routing,bot,safeRequest,environment,message,nullptr,false);
    auto task = load(taskId);
    task["workflowId"] = id;
    task["stageId"] = "implement";
    addRevision(task,safeRequest,safeRequest,message);
    save(task);
    exec("INSERT INTO workflow_steps(workflow_id,step,task_id) VALUES(?,'implement',?)",{id,taskId});
    return id;
}

Json Store::Impl::workflowTool(std::string_view tool, const Json& args, std::string_view bot,
                              Id message, const Json& routing, Seconds now, T3& t3) {
    if (tool=="start_workflow") {
        const auto id = startWorkflow(message,args,bot,routing,t3);
        auto status = workflowStatus(id,bot,now);
        status["effect"] = "started";
        return status;
    }
    const Id id = conversation_storage::identifier(args,"workflowId",true);
    if (tool=="workflow_status") return workflowStatus(id,bot,now);
    if (!query("SELECT 1 FROM workflows WHERE id=? AND bot_id=?", {id,bot}).next()) throw Conflict("Unknown workflow");
    exec("UPDATE workflows SET stop_requested=1 WHERE id=?", {id});
    auto children = query("SELECT task_id FROM workflow_steps WHERE workflow_id=?", {id});
    while (children.next()) {
        auto task = load(children.number(0));
        if (!terminal(task) && task.at("state")!="cancelling") requestCancellation(task);
    }
    auto status = workflowStatus(id,bot,now);
    status["effect"] = status.at("allStopped")==true ? "cancelled" : "cancelling";
    return status;
}

void Store::Impl::appendStageRequirements(std::string& instruction, const Json& task) const {
    const auto pin = field(task,"inputArtifactVersion");
    if (pin.empty()) {
        instruction += "\n\nCommit the implementation and return only JSON with artifactVersion (the full lowercase Git commit SHA). "
                       "The commit must be available in this T3 project's Git repository.";
        return;
    }
    auto workflow = query("SELECT request FROM workflows WHERE id=?", {task.at("workflowId")});
    if (!workflow.next()) throw Conflict("Missing workflow assignment");
    instruction += "\n\nRequested change: " + workflow.string(0) + "\n\nPinned input artifact version: " + pin +
        "\nVerify that exact version without modifying the tracked worktree. Return only JSON with artifactVersion and verdict (pass or fail).";
}

void Store::Impl::stageStarted(Id task, Seconds now) {
    exec(R"SQL(
UPDATE workflow_steps SET confirmed_state='running',confirmed_at=?,unreachable=0,status='running',
    deadline=CASE WHEN deadline=0 AND w.soft_deadline_seconds>0 THEN ?+w.soft_deadline_seconds ELSE deadline END
FROM workflows w WHERE w.id=workflow_steps.workflow_id AND task_id=?
)SQL",{now,now,task});
}

void Store::Impl::stageObservation(const Json& task, const Observation& observed, Seconds now) {
    const bool unreachable = observed.kind==Observation::Kind::Unavailable;
    const auto state = observed.kind==Observation::Kind::Finished ? observed.status :
                       observed.kind==Observation::Kind::Settling ? "settling" : "running";
    exec("UPDATE workflow_steps SET unreachable=?,confirmed_state=CASE WHEN ? THEN confirmed_state ELSE ? END,"
         "confirmed_at=CASE WHEN ? THEN confirmed_at ELSE ? END WHERE task_id=?",
         {int(unreachable),int(unreachable),state,int(unreachable),now,task.at("id")});
}

void Store::Impl::startVerification(const Json& task, Id reportId, const Json& report, const std::string& version) {
    const auto id = task.at("workflowId");
    auto row = query("SELECT verification_criteria,message_id,stop_requested FROM workflows WHERE id=?", {id});
    if (!row.next()) throw Conflict("Missing workflow assignment");
    if (row.number(2)) return;

    const auto bot = field(task,"botId");
    const auto scrub = redactor(bot);
    const auto criteria = scrub.text(row.string(0));
    const auto environment = field(task,"environmentId");
    Json routing = {{"environmentId",environment},{"projectId",task.at("projectId")},{"config",task.at("config")}};
    if (task.contains("sessionOrigin")) routing["sessionOrigin"] = task.at("sessionOrigin");
    const auto taskId = startConversationTask(routing,bot,criteria,environment,row.number(1),nullptr,false);
    auto verifier = load(taskId);
    verifier["workflowId"] = id;
    verifier["stageId"] = "verify";
    verifier["inputArtifactVersion"] = version;
    verifier["sourceResultRefs"] = scrub.record(Json::array({
        {{"stageId","implement"},{"artifactVersion",version},{"messageId",reportId},{"taskId",task.at("id")},
         {"run",report.at("run")},{"resultRunId",report.at("resultRunId")},{"text",bounded(field(report,"report"),2000)}}
    }),SecretRedactor::Fields::external);
    addRevision(verifier,criteria,criteria,row.number(1));
    save(verifier);
    exec("INSERT INTO workflow_steps(workflow_id,step,task_id) VALUES(?,'verify',?)",{id,taskId});
}

bool Store::Impl::settleStageReport(Json& task) {
    if (task.at("state")!="cancelling" && conversationPending(field(task,"botId"))) return false;
    const Id reportId = task.at("pendingStageReport");
    task.erase("pendingStageReport");
    auto row = query("SELECT data FROM journal WHERE id=? AND task_id=? AND kind='report'", {reportId,task.at("id")});
    if (!row.next()) throw Conflict("Missing workflow report");
    const auto report = row.json(0);
    const auto text = field(report,"report");
    const auto result = Json::parse(text,nullptr,false);
    const auto version = field(result,"artifactVersion");
    const auto pin = field(task,"inputArtifactVersion");
    const auto verdict = field(result,"verdict");
    const auto scrub = redactor(field(task,"botId"));
    const bool valid = field(report,"status")=="completed" && !report.value("truncated",false) &&
        field(report,"evidenceStatus")!="report_unavailable" && fullCommit(version) && scrub.text(version)==version &&
        (pin.empty() || (version==pin && (verdict=="pass" || verdict=="fail")));
    if (valid)
        exec("INSERT INTO workflow_evidence(report_id,version,verdict) VALUES(?,?,?)",
             {reportId,version,pin.empty() ? Json(nullptr) : Json(verdict)});
    if (task.at("state")=="cancelling" || task.value("replanning",false) ||
        !task.value("pendingInstructions",Json::array()).empty()) return true;

    const bool ok = valid && (pin.empty() || verdict=="pass");
    exec("UPDATE workflow_steps SET status=?,result_id=? WHERE task_id=?",
         {ok ? "completed" : "failed",reportId,task.at("id")});
    consumeEvent(task);
    task["state"] = "completed";
    const auto detail = text.empty() ? field(report,"detail") : text;
    journal(task.at("id"),"completed",{{"text",ok ? detail : "Workflow step failed: "+detail},
        {"revisionId",task.at("currentRevision")},{"source",report.at("run")}});
    if (ok && pin.empty()) startVerification(task,reportId,report,version);
    return true;
}
}
