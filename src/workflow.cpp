#include "conversation_storage.hpp"

#include <set>

namespace flockv0 {
namespace {
Seconds deadlineSeconds(const Json& args, Seconds inherited = 0) {
    if (!args.contains("softDeadlineSeconds")) return inherited;
    const auto& value = args.at("softDeadlineSeconds");
    if (!value.is_number_integer() || value<1 || value>31536000) throw Conflict("Invalid soft deadline");
    return value.get<Seconds>();
}

bool immutableVersion(std::string_view version) {
    if (const auto digest = version.find("@sha256:"); digest!=std::string_view::npos) {
        if (!digest || version.size()>1024 || version.find_first_of(" \r\n\t")!=std::string_view::npos) return false;
        version.remove_prefix(digest+1);
    }
    if (version.starts_with("sha256:")) {
        version.remove_prefix(7);
        if (version.size()!=64) return false;
    }
    return (version.size()==40 || version.size()==64) &&
           version.find_first_not_of("0123456789abcdef")==std::string_view::npos;
}

Json stageDefinitions(const Json& args) {
    const auto request = conversation_storage::text(args,"request",8000,true);
    const auto criteria = args.value("verificationCriteria",std::string("Verify the requested change."));
    Json stages = args.value("stages",Json::array({
        {{"stageId","implement"},{"role","implement"},{"request",request}},
        {{"stageId","verify"},{"role","verify"},{"request",criteria},{"dependsOn",{"implement"}}}}));
    if (!stages.is_array() || stages.empty() || stages.size()>32) throw Conflict("Specify 1 to 32 workflow stages");
    const auto deadline = deadlineSeconds(args);
    std::set<std::string> ids;
    for (auto& stage : stages) {
        const auto id = conversation_storage::text(stage,"stageId",80,true);
        if (!ids.insert(id).second) throw Conflict("Duplicate stageId");
        const auto role = conversation_storage::text(stage,"role",80,true);
        stage["name"] = stage.value("name",id);
        conversation_storage::text(stage,"name",160,true);
        stage["request"] = stage.value("request",role=="verify" ? criteria : request);
        conversation_storage::text(stage,"request",8000,true);
        stage["softDeadlineSeconds"] = deadlineSeconds(stage,deadline);
        auto deps = stage.value("dependsOn",Json::array());
        if (!deps.is_array() || deps.size()>32) throw Conflict("Invalid stage dependencies");
        std::set<std::string> unique;
        for (const auto& dep : deps)
            if (!dep.is_string() || !unique.insert(dep.get<std::string>()).second)
                throw Conflict("Each dependency must name a distinct stage");
        if (role=="verify" && deps.empty()) throw Conflict("Verification needs an artifact-producing dependency");
        if (deps.size()==1 && !stage.contains("inputFrom")) stage["inputFrom"] = deps.front();
        if ((role=="verify" || stage.contains("inputFrom")) &&
            std::ranges::find(deps,Json(field(stage,"inputFrom")))==deps.end())
            throw Conflict("Choose inputFrom among the stage dependencies");
        Json spec = {{"stageId",id},{"name",stage.at("name")},{"role",role},{"request",stage.at("request")},
                     {"dependsOn",deps},{"softDeadlineSeconds",stage.at("softDeadlineSeconds")}};
        if (stage.contains("inputFrom")) spec["inputFrom"] = stage.at("inputFrom");
        stage = std::move(spec);
    }
    std::set<std::string> ready;
    while (ready.size()<ids.size()) {
        const auto prior = ready.size();
        for (const auto& stage : stages) {
            if (std::ranges::all_of(stage.at("dependsOn"),[&](const Json& dep) {
                return ready.contains(dep.get<std::string>());
            })) ready.insert(field(stage,"stageId"));
        }
        if (prior==ready.size()) throw Conflict("Unknown or cyclic workflow dependency");
    }
    return stages;
}

bool closed(std::string_view status) {
    return status=="completed" || status=="failed" || status=="stopped";
}
}

void Store::Impl::initializeWorkflowSchema() {
    exec(R"SQL(CREATE TABLE IF NOT EXISTS workflows(
        id INTEGER PRIMARY KEY,bot_id TEXT NOT NULL,routing TEXT NOT NULL,request TEXT NOT NULL,
        message_id INTEGER NOT NULL REFERENCES messages,stop_requested INTEGER NOT NULL DEFAULT 0))SQL");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS workflow_stages(workflow_id INTEGER NOT NULL REFERENCES workflows,
        stage_id TEXT NOT NULL,spec TEXT NOT NULL,task_id INTEGER UNIQUE REFERENCES tasks,result_id INTEGER REFERENCES journal,instruction TEXT,status TEXT NOT NULL DEFAULT 'pending',
        input TEXT NOT NULL DEFAULT '',output TEXT NOT NULL DEFAULT '',deadline INTEGER NOT NULL DEFAULT 0, alerted INTEGER NOT NULL DEFAULT 0,
        confirmed_state TEXT NOT NULL DEFAULT '',confirmed_at INTEGER NOT NULL DEFAULT 0,unreachable INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY(workflow_id,stage_id)))SQL");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS workflow_dependencies(
        workflow_id INTEGER NOT NULL,stage_id TEXT NOT NULL,parent_id TEXT NOT NULL,report_id INTEGER REFERENCES journal,
        artifact_version TEXT NOT NULL DEFAULT '',
        PRIMARY KEY(workflow_id,stage_id,parent_id),
        FOREIGN KEY(workflow_id,stage_id) REFERENCES workflow_stages,
        FOREIGN KEY(workflow_id,parent_id) REFERENCES workflow_stages))SQL");
    exec(R"SQL(CREATE VIEW IF NOT EXISTS outdated_workflow_stages AS
        WITH RECURSIVE outdated(workflow_id,stage_id) AS (
          SELECT d.workflow_id,d.stage_id FROM workflow_dependencies d JOIN workflow_stages p
            ON p.workflow_id=d.workflow_id AND p.stage_id=d.parent_id
            WHERE d.report_id IS NOT NULL AND (p.status<>'completed' OR p.output<>d.artifact_version
              OR (p.result_id IS NOT d.report_id AND
                  (d.artifact_version='' OR json_extract(p.spec,'$.role') NOT IN ('implement','verify'))))
          UNION
          SELECT d.workflow_id,d.stage_id FROM workflow_dependencies d JOIN outdated p
            ON p.workflow_id=d.workflow_id AND p.stage_id=d.parent_id)
        SELECT * FROM outdated)SQL");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS workflow_artifacts(
        report_id INTEGER PRIMARY KEY REFERENCES journal,workflow_id INTEGER NOT NULL,stage_id TEXT NOT NULL,
        version TEXT NOT NULL,FOREIGN KEY(workflow_id,stage_id) REFERENCES workflow_stages))SQL");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS workflow_verdicts(
        report_id INTEGER PRIMARY KEY REFERENCES journal,workflow_id INTEGER NOT NULL,stage_id TEXT NOT NULL,
        version TEXT NOT NULL,verdict TEXT NOT NULL CHECK(verdict IN ('pass','fail')),
        FOREIGN KEY(workflow_id,stage_id) REFERENCES workflow_stages))SQL");
}

Json Store::Impl::workflowStatus(Id id, std::string_view bot, Seconds now) const {
    auto owner = query("SELECT stop_requested,request FROM workflows WHERE id=? AND bot_id=?",{id,bot});
    if (!owner.next()) throw Conflict("Unknown workflow");
    const auto scrub = redactor(bot);
    const bool stopping = owner.number(0);
    Json result = {{"ok",true},{"workflowId",id},{"request",owner.string(1)},{"stages",Json::array()},{"artifacts",Json::array()},{"verdicts",Json::array()}};
    bool finished = true, attention = false, failed = false, stale = false;
    auto rows = query(R"SQL(SELECT s.spec,s.status,s.task_id,s.input,s.output,s.deadline,t.data,t.state,s.alerted,
          s.confirmed_state,s.confirmed_at,s.unreachable,old.stage_id IS NOT NULL,
          (SELECT json_group_array(d.parent_id) FROM workflow_dependencies d WHERE d.workflow_id=s.workflow_id AND d.stage_id=s.stage_id),
          pending.phase='started' AND json_extract(t.data,'$.activeRun') IS NULL,v.verdict
        FROM workflow_stages s LEFT JOIN tasks t ON t.id=s.task_id
        LEFT JOIN outbox pending ON pending.id=json_extract(t.data,'$.actionId')
        LEFT JOIN outdated_workflow_stages old USING(workflow_id,stage_id)
        LEFT JOIN workflow_verdicts v ON v.report_id=s.result_id
        WHERE s.workflow_id=? ORDER BY s.rowid)SQL",{id});
    while (rows.next()) {
        const auto spec = rows.json(0);
        const auto taskId = rows.number(2);
        const auto task = taskId ? rows.json(6) : Json::object();
        auto status = rows.string(1);
        if (rows.string(7)=="cancelled") status = "stopped";
        else if (!closed(status) && (rows.number(11) || rows.number(14))) status = "unreachable";
        else if (rows.string(7)=="cancelling") status = "stop_requested";
        const auto active = task.value("activeRun",Json(nullptr));
        const auto run = active.is_object() ? active : task.value("lastRun",Json(nullptr));
        const bool exceeded = !closed(status) && rows.number(5)>0 && (now>=rows.number(5) || rows.number(8));
        const bool outdated = closed(status) && rows.number(12);
        const bool needs = outdated || exceeded || status=="failed" || status=="unreachable" ||
            rows.string(7)=="waiting" || task.contains("pendingRunReport");
        const bool childSettled = !taskId || terminal(task);
        finished &= closed(status) && childSettled && active.is_null() && !rows.number(14);
        attention |= needs;
        failed |= (status=="failed" && !outdated) || status=="stopped";
        stale |= outdated;
        Json stage = {{"stageId",spec.at("stageId")},{"name",spec.at("name")},{"role",scrub.text(field(spec,"role"))},
            {"dependsOn",rows.json(13)},{"outdated",outdated},{"taskId",taskId},{"runRef",run},{"status",status},
            {"stopRequested",stopping || rows.string(7)=="cancelling" || status=="stopped"},
            {"softDeadlineAt",rows.number(5)},{"deadlineExceeded",exceeded},{"needsAttention",needs},
            {"lastConfirmedState",rows.string(9)},{"lastConfirmedAt",rows.number(10)},
            {"inputArtifactVersion",rows.string(3)},{"outputArtifactVersion",rows.string(4)}};
        if (spec.at("role")=="verify") stage["verified"] = status=="completed" && !outdated;
        const auto verdict = rows.string(15);
        if (!verdict.empty() && !outdated && (status=="completed" || status=="failed")) stage["currentVerdict"] = verdict;
        result["stages"].push_back(stage);
    }
    auto evidence = query(R"SQL(SELECT e.stage_id,j.task_id,e.version,e.verdict,j.id,j.data FROM (
        SELECT stage_id,version,report_id,NULL AS verdict FROM workflow_artifacts WHERE workflow_id=?
        UNION ALL SELECT stage_id,version,report_id,verdict FROM workflow_verdicts WHERE workflow_id=?) e
        JOIN journal j ON j.id=e.report_id ORDER BY j.id)SQL",{id,id});
    while (evidence.next()) {
        const auto report = evidence.json(5);
        Json item = {{"stageId",evidence.string(0)},{"taskId",evidence.number(1)},{"artifactVersion",evidence.string(2)},
                     {"messageId",evidence.number(4)},{"runRef",report.at("run")},{"resultRunId",report.at("resultRunId")},
                     {"itemId",report.at("itemId")}};
        if (!evidence.string(3).empty()) item["verdict"] = evidence.string(3);
        auto& records = result[item.contains("verdict") ? "verdicts" : "artifacts"];
        records.push_back(std::move(item));
    }
    result["status"] = stopping ? (finished ? "stopped" : "stop_requested") :
        failed ? "failed" : stale ? "needs_verification" : finished ? "completed" : "running";
    result["allStopped"] = stopping && finished;
    result["needsAttention"] = attention || (!stopping && failed);
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

Id Store::Impl::startWorkflow(Id message, const Json& args, std::string_view bot, const Json& routing) {
    const auto environment = taskEnvironment(bot,routing);
    if (environment.empty()) throw Conflict("Choose a connected T3 environment for this workflow");
    auto destination = routing;
    destination["environmentId"] = environment;
    const auto scrub = redactor(bot);
    scrub.requirePublic({bot,destination});
    const auto safe = scrub.record(args,SecretRedactor::Fields::external);
    const auto stages = stageDefinitions(safe);
    exec("INSERT INTO workflows(bot_id,routing,request,message_id) VALUES(?,?,?,?)",
         {bot,destination,safe.at("request"),message});
    const auto id = last();
    for (auto stage : stages) {
        stage.erase("dependsOn");
        exec("INSERT INTO workflow_stages(workflow_id,stage_id,spec) VALUES(?,?,?)",{id,stage.at("stageId"),stage});
    }
    for (const auto& stage : stages)
        for (const auto& parent : stage.at("dependsOn"))
            exec("INSERT INTO workflow_dependencies(workflow_id,stage_id,parent_id) VALUES(?,?,?)",{id,stage.at("stageId"),parent});
    return id;
}

Json Store::Impl::workflowTool(std::string_view tool, const Json& args, std::string_view bot, Id message,
                              int step, const Json& routing) {
    if (tool=="start_workflow") {
        const auto id = startWorkflow(message,args,bot,routing);
        auto status = workflowStatus(id,bot,unixTime());
        status["effect"] = "started";
        return status;
    }
    const Id id = conversation_storage::identifier(args,"workflowId",true);
    if (tool=="workflow_status") return workflowStatus(id,bot,unixTime());
    auto owner = query("SELECT stop_requested FROM workflows WHERE id=? AND bot_id=?", {id,bot});
    if (!owner.next()) throw Conflict("Unknown workflow");
    const bool stopping = owner.number(0);

    const bool whole = tool=="stop_workflow";
    const auto target = whole ? std::string{} : conversation_storage::text(args,"stageId",80,true);
    if (whole || tool=="stop_workflow_stage") {
        if (whole) exec("UPDATE workflows SET stop_requested=1 WHERE id=?", {id});
        auto children = query("SELECT task_id FROM workflow_stages WHERE workflow_id=? AND (?='' OR stage_id=?)", {id,target,target});
        if (!children.next()) throw Conflict("Unknown stage");
        bool awaiting = false;
        do {
            if (const Id taskId = children.number(0)) {
                auto task = load(taskId);
                if (!terminal(task) && task.at("state")!="cancelling") requestCancellation(task);
                awaiting |= !terminal(task);
            }
        } while (children.next());
        exec("UPDATE workflow_stages SET status='stopped' WHERE workflow_id=? AND (?='' OR stage_id=?)"
             " AND task_id IS NULL AND status='pending'", {id,target,target});
        auto status = workflowStatus(id,bot,unixTime());
        status["effect"] = awaiting ? "cancelling" : "cancelled";
        if (!whole) status["stageId"] = target;
        return status;
    }

    if (stopping) throw Conflict("Workflow is stopping");
    const auto instruction = redactor(bot).text(conversation_storage::text(args,"instruction",8000,true));
    auto stage = query("SELECT s.task_id,s.status,EXISTS(SELECT 1 FROM workflow_dependencies d WHERE d.workflow_id=s.workflow_id"
                       " AND d.stage_id=s.stage_id) FROM workflow_stages s WHERE s.workflow_id=? AND s.stage_id=?", {id,target});
    if (!stage.next()) throw Conflict("Unknown stage");
    const Id taskId = stage.number(0);
    auto task = taskId ? load(taskId) : Json(nullptr);
    if (stage.string(1)=="stopped" || (taskId && (task.at("state")=="cancelled" || task.at("state")=="cancelling")))
        throw Conflict("Stage is stopping");
    if (stage.number(2) && (stage.string(1)=="failed" || (taskId && terminal(task)))) {
        Json next = {{"text",instruction},{"messageId",message},{"step",step}};
        if (taskId) next["priorRevisionId"] = task.at("currentRevision");
        exec("UPDATE workflow_stages SET task_id=NULL,status='pending',deadline=0,alerted=0,input='',output='',"
             "result_id=NULL,confirmed_at=0,confirmed_state='',unreachable=0,"
             "instruction=json_patch(coalesce(instruction,'{}'),?) WHERE workflow_id=? AND stage_id=?",
             {next,id,target});
    } else {
        if (!taskId) throw Conflict("Stage has no assigned task yet");
        if (terminal(task)) {
            task["state"] = "queued";
            exec("UPDATE workflow_stages SET status='pending',deadline=0,alerted=0 WHERE workflow_id=? AND stage_id=?", {id,target});
        }
        acceptInstruction(task,message,step,instruction,nullptr,nullptr);
    }
    auto status = workflowStatus(id,bot,unixTime());
    status["effect"] = "accepted";
    status["stageId"] = target;
    return status;
}

void Store::Impl::advanceWorkflows(Seconds now) {
    std::lock_guard lock(mutex);
    Transaction tx(db);
    auto overdue = query("SELECT s.task_id FROM workflow_stages s JOIN tasks t ON t.id=s.task_id "
        "WHERE s.deadline BETWEEN 1 AND ? AND s.alerted=0 AND t.state NOT IN ('completed','cancelled')",{now});
    while (overdue.next()) {
        const auto task = overdue.number(0);
        exec("UPDATE workflow_stages SET alerted=1 WHERE task_id=?",{task});
        journal(task,"workflow_deadline_exceeded",{{"needsAttention",true},{"observedAt",now}});
    }
    materializeWorkflowStages();
    tx.commit();
}

void Store::Impl::materializeWorkflowStages() {
    auto ready = query(R"SQL(WITH ready AS MATERIALIZED (
        SELECT s.workflow_id,s.spec,w.bot_id,w.routing,w.message_id,s.instruction,
               r.id,r.intent,r.done_when,
               coalesce(r.source_message_id,w.message_id)
        FROM workflow_stages s JOIN workflows w ON w.id=s.workflow_id
        LEFT JOIN task_revisions r ON r.id=json_extract(s.instruction,'$.priorRevisionId')
        WHERE w.stop_requested=0 AND s.status='pending' AND s.task_id IS NULL AND NOT EXISTS (
          SELECT 1 FROM workflow_dependencies d JOIN workflow_stages p ON p.workflow_id=d.workflow_id AND p.stage_id=d.parent_id
          LEFT JOIN outdated_workflow_stages old ON old.workflow_id=p.workflow_id AND old.stage_id=p.stage_id
          WHERE d.workflow_id=s.workflow_id AND d.stage_id=s.stage_id AND (p.status<>'completed' OR old.stage_id IS NOT NULL)))
        SELECT * FROM ready)SQL");
    while (ready.next()) {
        const Id id = ready.number(0);
        const auto spec = ready.json(1);
        const auto stage = spec.at("stageId");
        const auto bot = ready.string(2);
        const auto scrub = redactor(bot);
        const auto routing = ready.json(3);
        const auto request = scrub.text(field(spec,"request"));
        Json sources = Json::array();
        std::string input;
        const auto inputFrom = field(spec,"inputFrom");
        auto parents = query(R"SQL(SELECT p.stage_id,p.output,p.result_id,p.task_id,j.data
            FROM workflow_dependencies d JOIN workflow_stages p ON p.workflow_id=d.workflow_id AND p.stage_id=d.parent_id
            JOIN journal j ON j.id=p.result_id WHERE d.workflow_id=? AND d.stage_id=? ORDER BY d.rowid)SQL",{id,stage});
        while (parents.next()) {
            const auto report = parents.json(4);
            if (parents.string(0)==inputFrom) input = parents.string(1);
            sources.push_back({{"stageId",parents.string(0)},{"artifactVersion",parents.string(1)},
                {"messageId",parents.number(2)},{"taskId",parents.number(3)},{"run",report.at("run")},
                {"resultRunId",report.at("resultRunId")},{"text",bounded(scrub.text(field(report,"report")),2000)}});
        }
        const auto role = field(spec,"role");
        if (!conversation_storage::hasText(request) || scrub.text(input)!=input ||
            (role=="verify" && !immutableVersion(input)) || scrub.text(bot)!=bot ||
            scrub.record(routing,SecretRedactor::Fields::external)!=routing) {
            exec("UPDATE workflow_stages SET status='failed' WHERE workflow_id=? AND stage_id=?",{id,spec.at("stageId")});
            continue;
        }
        exec(R"SQL(UPDATE workflow_dependencies AS d SET report_id=p.result_id,artifact_version=p.output
            FROM workflow_stages p WHERE p.workflow_id=d.workflow_id AND p.stage_id=d.parent_id
            AND d.workflow_id=? AND d.stage_id=?)SQL",{id,stage});
        const TaskRoute route{field(routing,"environmentId"),field(routing,"projectId")};
        const auto taskId = startConversationTask(routing,bot,request,route,ready.number(4),nullptr,false);
        auto task = load(taskId);
        task["workflowId"] = id;
        task["stageId"] = spec.at("stageId");
        task["sourceResultRefs"] = scrub.record(std::move(sources),SecretRedactor::Fields::external);
        task["inputArtifactVersion"] = input;
        const bool hasPriorRevision = ready.number(6) != 0;
        addRevision(task,hasPriorRevision ? scrub.text(ready.string(7)) : request,
                    hasPriorRevision ? scrub.text(ready.string(8)) : request,
                    ready.number(9));
        if (!ready.string(5).empty()) {
            const auto next = ready.json(5);
            acceptInstruction(task,next.at("messageId"),next.at("step"),
                              scrub.text(field(next,"text")),nullptr,nullptr);
        } else save(task);
        exec("UPDATE workflow_stages SET task_id=?,input=?,instruction=NULL WHERE workflow_id=? AND stage_id=?",
             {taskId,input,id,spec.at("stageId")});
    }
}

void Store::Impl::appendStageRequirements(std::string& instruction, const Json& task) const {
    auto stage = query("SELECT s.spec,s.input,w.request FROM workflow_stages s"
                       " JOIN workflows w ON w.id=s.workflow_id WHERE s.task_id=?", {task.at("id")});
    if (!stage.next()) throw Conflict("Missing workflow stage assignment");
    const auto spec = stage.json(0);
    const auto role = field(spec,"role"), pin = stage.string(1);
    if (role=="verify") instruction += "\n\nRequested change: " + stage.string(2);
    if (!pin.empty()) instruction += "\n\nPinned input artifact version: " + pin;
    if (role=="verify") instruction += "\nVerify that exact version. Return only JSON with artifactVersion and verdict (pass or fail).";
    else if (role=="implement") instruction += "\n\nReturn only JSON with the immutable artifactVersion (full lowercase commit SHA or SHA-256 digest).";
}

void Store::Impl::stageObservation(const Json& task, const Observation& observed, Seconds now) {
    const bool unreachable = observed.kind==Observation::Kind::Unavailable;
    const auto state = observed.kind == Observation::Kind::Finished ? observed.status :
                       observed.kind == Observation::Kind::Settling ? "settling" : "running";
    exec("UPDATE workflow_stages SET unreachable=?,confirmed_state=CASE WHEN ? THEN confirmed_state ELSE ? END,"
         "confirmed_at=CASE WHEN ? THEN confirmed_at ELSE ? END WHERE task_id=?",
         {int(unreachable),int(unreachable),state,int(unreachable),now,task.at("id")});
}

bool Store::Impl::settleStageReport(Json& task) {
    if (task.at("state")!="cancelling" && conversationPending(field(task,"botId"))) return false;
    const Id reportId = task.at("pendingStageReport");
    task.erase("pendingStageReport");
    const auto id = task.at("workflowId");
    const auto stage = task.at("stageId");
    auto row = query("SELECT s.spec,s.input,j.data FROM workflow_stages s JOIN journal j ON j.id=? AND j.task_id=s.task_id"
                     " WHERE workflow_id=? AND stage_id=? AND s.task_id=?", {reportId,id,stage,task.at("id")});
    if (!row.next()) throw Conflict("Missing workflow stage assignment");
    const auto role = field(row.json(0),"role");
    const auto pin = row.string(1);
    const auto report = row.json(2);
    const auto result = Json::parse(field(report,"report"),nullptr,false);
    const auto version = field(result,"artifactVersion");
    bool ok = field(report,"status")=="completed" && !report.value("truncated",false) &&
        field(report,"evidenceStatus") != "report_unavailable";
    std::string output;
    if (role=="verify") {
        const auto verdict = field(result,"verdict");
        ok = ok && version==pin && (verdict=="pass" || verdict=="fail");
        if (ok) exec("INSERT INTO workflow_verdicts(workflow_id,stage_id,report_id,version,verdict) VALUES(?,?,?,?,?)",
                     {id,stage,reportId,pin,verdict});
        ok = ok && verdict=="pass";
        if (ok) output = pin;
    } else if (role=="implement" || !version.empty()) {
        ok = ok && immutableVersion(version);
        if (ok) {
            output = version;
            exec("INSERT INTO workflow_artifacts(workflow_id,stage_id,report_id,version) VALUES(?,?,?,?)",
                 {id,stage,reportId,version});
        }
    } else if (ok) output = pin;
    if (task.at("state")=="cancelling" || task.value("replanning",false) ||
        !task.value("pendingInstructions",Json::array()).empty()) return true;
    exec("UPDATE workflow_stages SET status=?,output=?,result_id=? WHERE workflow_id=? AND stage_id=?",
         {ok ? "completed" : "failed",output,reportId,id,stage});
    consumeEvent(task);
    task["state"] = "completed";
    const auto text = field(report,"evidenceStatus") == "report_unavailable" ? field(report,"detail") : field(report,"report");
    Json completion = {{"text",ok ? text : "Workflow stage failed: "+text},
        {"revisionId",task.at("currentRevision")},{"source",report.at("run")}};
    if (field(report,"status") == "completed") completion["workflowOutcome"] = ok ? "completed" : "failed";
    journal(task.at("id"),"completed",std::move(completion));
    return true;
}
}
