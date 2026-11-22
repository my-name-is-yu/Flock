#include "conversation_storage.hpp"

#include <set>

namespace flockv0 {
namespace {
std::string requiredText(const Json& object, const char* key, std::size_t limit) {
    const auto text = field(object,key);
    if (!conversation_storage::hasText(text) || text.size()>limit) throw Conflict(std::string("Invalid ")+key);
    return text;
}

Seconds deadlineSeconds(const Json& args, Seconds fallback = 0) {
    if (!args.contains("softDeadlineSeconds")) return fallback;
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
    const auto request = requiredText(args,"request",8000);
    const auto criteria = args.value("verificationCriteria",std::string("Verify the requested change."));
    Json stages = args.value("stages",Json::array({
        {{"stageId","implement"},{"role","implement"},{"request",request}},
        {{"stageId","verify"},{"role","verify"},{"request",criteria},{"dependsOn",{"implement"}}}}));
    if (!stages.is_array() || stages.empty() || stages.size()>32) throw Conflict("Specify 1 to 32 workflow stages");
    std::set<std::string> ids;
    for (auto& stage : stages) {
        const auto id = requiredText(stage,"stageId",80);
        if (!ids.insert(id).second) throw Conflict("Duplicate stageId");
        const auto role = requiredText(stage,"role",80);
        stage["name"] = stage.value("name",id);
        requiredText(stage,"name",160);
        stage["request"] = stage.value("request",role=="verify" ? criteria : request);
        requiredText(stage,"request",8000);
        stage["softDeadlineSeconds"] = deadlineSeconds(stage,deadlineSeconds(args));
        auto deps = stage.value("dependsOn",Json::array());
        if (!deps.is_array() || deps.size()>32) throw Conflict("Invalid stage dependencies");
        stage["dependsOn"] = deps;
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
                return dep.is_string() && ready.contains(dep.get<std::string>());
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

void Store::Impl::migrateWorkflow() {
    exec(R"SQL(CREATE TABLE IF NOT EXISTS workflows(id INTEGER PRIMARY KEY,bot_id TEXT NOT NULL,
        routing TEXT NOT NULL,message_id INTEGER NOT NULL,stop_requested INTEGER NOT NULL DEFAULT 0))SQL");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS workflow_stages(workflow_id INTEGER NOT NULL REFERENCES workflows,
        stage_id TEXT NOT NULL,spec TEXT NOT NULL,task_id INTEGER REFERENCES tasks,status TEXT NOT NULL DEFAULT 'pending',
        input TEXT NOT NULL DEFAULT '',output TEXT NOT NULL DEFAULT '',deadline INTEGER NOT NULL DEFAULT 0, alerted INTEGER NOT NULL DEFAULT 0,
        confirmed_state TEXT NOT NULL DEFAULT '',confirmed_at INTEGER NOT NULL DEFAULT 0,unreachable INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY(workflow_id,stage_id)))SQL");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS workflow_artifacts(id INTEGER PRIMARY KEY,workflow_id INTEGER NOT NULL,
        stage_id TEXT NOT NULL,task_id INTEGER NOT NULL REFERENCES tasks,version TEXT NOT NULL,run TEXT NOT NULL))SQL");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS workflow_verdicts(id INTEGER PRIMARY KEY,workflow_id INTEGER NOT NULL,
        stage_id TEXT NOT NULL,task_id INTEGER NOT NULL REFERENCES tasks,version TEXT NOT NULL,verdict TEXT NOT NULL,
        run TEXT NOT NULL))SQL");
}

Json Store::Impl::workflowStatus(Id id, std::string_view bot, Seconds now) const {
    auto owner = query("SELECT stop_requested FROM workflows WHERE id=? AND bot_id=?",{id,bot});
    if (!owner.next()) throw Conflict("Unknown workflow");
    const bool stopping = owner.number(0);
    Json result = {{"ok",true},{"workflowId",id},{"stages",Json::array()},{"artifacts",Json::array()},{"verdicts",Json::array()}};
    bool finished = true, attention = false, failed = false, stale = false;
    auto rows = query("SELECT s.spec,s.status,s.task_id,s.input,s.output,s.deadline,t.data,t.state,s.alerted,s.confirmed_state,s.confirmed_at,s.unreachable "
        "FROM workflow_stages s LEFT JOIN tasks t ON t.id=s.task_id WHERE s.workflow_id=? ORDER BY s.rowid",{id});
    while (rows.next()) {
        const auto spec = rows.json(0);
        const auto task = rows.number(2) ? rows.json(6) : Json::object();
        auto status = rows.string(1);
        const auto waitReason = field(task.value("wait",Json(nullptr)),"reason");
        if (rows.string(7)=="cancelled") status = "stopped";
        else if (!closed(status) && (rows.number(11) || waitReason=="unknown_delivery")) status = "unreachable";
        else if (rows.string(7)=="cancelling") status = "stop_requested";
        const auto active = task.value("activeRun",Json(nullptr));
        const auto run = active.is_object() ? active : task.value("lastRun",Json(nullptr));
        const bool exceeded = !closed(status) && rows.number(5)>0 && (now>=rows.number(5) || rows.number(8));
        const bool needs = exceeded || status=="failed" || status=="unreachable" || rows.string(7)=="waiting";
        finished &= closed(status);
        attention |= needs;
        failed |= status=="failed" || status=="stopped";
        Json stage = {{"stageId",spec.at("stageId")},{"name",spec.at("name")},{"role",spec.at("role")},
            {"dependsOn",spec.at("dependsOn")},{"taskId",rows.number(2)},{"runRef",run},{"status",status},
            {"stopRequested",stopping || rows.string(7)=="cancelling" || status=="stop_requested"},
            {"softDeadlineAt",rows.number(5)},{"deadlineExceeded",exceeded},{"needsAttention",needs},
            {"lastConfirmedState",rows.string(9)},{"lastConfirmedAt",rows.number(10)},
            {"inputArtifactVersion",rows.string(3)},{"outputArtifactVersion",rows.string(4)}};
        if (spec.at("role")=="verify") {
            auto output = query("SELECT output,status FROM workflow_stages WHERE workflow_id=? AND stage_id=?",
                                {id,spec.at("inputFrom")});
            output.next();
            const bool current = status=="completed" && output.string(1)=="completed" && rows.string(3)==output.string(0);
            stage["verified"] = current;
            stale |= status=="completed" && !current;
        }
        result["stages"].push_back(stage);
    }
    auto artifacts = query("SELECT stage_id,task_id,version,run FROM workflow_artifacts WHERE workflow_id=? ORDER BY id",{id});
    while (artifacts.next()) result["artifacts"].push_back({{"stageId",artifacts.string(0)},{"taskId",artifacts.number(1)},
        {"artifactVersion",artifacts.string(2)},{"runRef",artifacts.json(3)}});
    auto verdicts = query("SELECT stage_id,task_id,version,verdict,run FROM workflow_verdicts WHERE workflow_id=? ORDER BY id",{id});
    while (verdicts.next()) result["verdicts"].push_back({{"stageId",verdicts.string(0)},{"taskId",verdicts.number(1)},
        {"artifactVersion",verdicts.string(2)},{"verdict",verdicts.string(3)},{"runRef",verdicts.json(4)}});
    result["status"] = stopping ? (finished ? "stopped" : "stop_requested") :
        failed ? "failed" : stale ? "needs_verification" : finished ? "completed" : "running";
    result["allStopped"] = stopping && finished;
    result["needsAttention"] = attention || stale || (!stopping && failed);
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

Json Store::Impl::workflowTool(std::string_view tool, const Json& args, std::string_view bot, Id message,
                              int step, const Json& routing) {
    if (tool=="start_workflow") {
        requiredText(routing,"environmentId",1024);
        requiredText(routing,"projectId",1024);
        const auto stages = stageDefinitions(args);
        exec("INSERT INTO workflows(bot_id,routing,message_id) VALUES(?,?,?)",{bot,routing,message});
        const auto id = last();
        for (const auto& stage : stages)
            exec("INSERT INTO workflow_stages(workflow_id,stage_id,spec) VALUES(?,?,?)",{id,stage.at("stageId"),stage});
        return workflowStatus(id,bot,unixTime());
    }
    const Id id = args.at("workflowId").get<Id>();
    auto status = workflowStatus(id,bot,unixTime());
    if (tool=="workflow_status") return status;
    const bool whole = tool=="stop_workflow";
    const auto target = whole ? "" : requiredText(args,"stageId",80);
    if (whole) exec("UPDATE workflows SET stop_requested=1 WHERE id=?",{id});
    bool found = false;
    for (auto& stage : status.at("stages")) {
        if (!whole && stage.at("stageId")!=target) continue;
        found = true;
        const Id taskId = stage.at("taskId");
        const auto state = field(stage,"status");
        if (tool=="instruct_workflow_stage") {
            const auto instruction = requiredText(args,"instruction",8000);
            if (!taskId || stage.at("stopRequested")==true || state=="stopped") throw Conflict("Stage is pending or stopping");
            auto task = load(taskId);
            if (terminal(task) && stage.at("role")=="verify") {
                exec("UPDATE workflow_stages SET task_id=NULL,status='pending',deadline=0,alerted=0,input='',output='',"
                     "confirmed_at=0,confirmed_state='',unreachable=0,spec=json_set(spec,'$.nextInstruction',?,'$.instructionMessage',?,'$.instructionStep',?) "
                     "WHERE workflow_id=? AND stage_id=?", {instruction,message,step,id,target});
                continue;
            }
            if (terminal(task)) {
                task["state"] = "deciding";
                save(task);
                exec("UPDATE workflow_stages SET status='pending',deadline=0,alerted=0 WHERE workflow_id=? AND stage_id=?",{id,target});
            }
            acceptInstruction(taskId,task.value("currentRevision",Id{}),message,step,instruction,nullptr,routing,nullptr);
        } else if (!closed(state)) {
            if (taskId) {
                auto task = load(taskId);
                if (!terminal(task) && task.at("state")!="cancelling") requestCancellation(task);
            }
            exec("UPDATE workflow_stages SET status=? WHERE workflow_id=? AND stage_id=?",
                 {taskId ? "stop_requested" : "stopped",id,stage.at("stageId")});
        }
    }
    if (!found) throw Conflict("Unknown stage");
    return workflowStatus(id,bot,unixTime());
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
    auto ready = query(R"SQL(SELECT s.workflow_id,s.spec,w.bot_id,w.routing,w.message_id,coalesce(source.output,'')
        FROM workflow_stages s JOIN workflows w ON w.id=s.workflow_id
        LEFT JOIN workflow_stages source ON source.workflow_id=w.id AND source.stage_id=json_extract(s.spec,'$.inputFrom')
        WHERE w.stop_requested=0 AND s.status='pending' AND s.task_id IS NULL AND NOT EXISTS (
          SELECT 1 FROM json_each(s.spec,'$.dependsOn') d JOIN workflow_stages parent
          ON parent.workflow_id=w.id AND parent.stage_id=d.value
          LEFT JOIN workflow_stages producer ON producer.workflow_id=w.id AND producer.stage_id=json_extract(parent.spec,'$.inputFrom')
          WHERE parent.status<>'completed' OR (json_extract(parent.spec,'$.role')='verify' AND
            (parent.input<>producer.output OR producer.status<>'completed'))))SQL");
    while (ready.next()) {
        const Id id = ready.number(0);
        const auto spec = ready.json(1);
        const auto input = ready.string(5);
        const auto role = field(spec,"role");
        if (role=="verify" && !immutableVersion(input)) {
            exec("UPDATE workflow_stages SET status='failed' WHERE workflow_id=? AND stage_id=?",{id,spec.at("stageId")});
            continue;
        }
        auto request = field(spec,"request");
        if (!input.empty()) request += "\nPinned input artifact version: " + input;
        if (role=="verify") request += "\nVerify that exact version. Return only JSON with artifactVersion and verdict (pass or fail).";
        else if (role=="implement") request += "\nReturn only JSON with the immutable artifactVersion (full lowercase commit SHA or SHA-256 digest).";
        const auto taskId = startConversationTask(ready.json(3),ready.string(2),request,"",ready.number(4),nullptr,false);
        auto task = load(taskId);
        task["workflowId"] = id;
        task["stageId"] = spec.at("stageId");
        task["stageAssignment"] = request;
        task["inputArtifactVersion"] = input;
        addRevision(task,field(spec,"request"),request,ready.number(4));
        save(task);
        if (spec.contains("nextInstruction"))
            acceptInstruction(taskId,task.at("currentRevision"),spec.at("instructionMessage"),spec.at("instructionStep"),
                              field(spec,"nextInstruction"),nullptr,ready.json(3),nullptr);
        exec("UPDATE workflow_stages SET task_id=?,input=? WHERE workflow_id=? AND stage_id=?",
             {taskId,input,id,spec.at("stageId")});
    }
    tx.commit();
}

void Store::Impl::stageObservation(const Json& task, const Observation& observed, Seconds now) {
    std::lock_guard lock(mutex);
    const bool unreachable = observed.kind==Observation::Kind::Unavailable;
    const auto state = observed.kind!=Observation::Kind::Finished ? "running" :
                       observed.settled ? observed.status : "settling";
    exec("UPDATE workflow_stages SET unreachable=?,confirmed_state=CASE WHEN ? THEN confirmed_state ELSE ? END,"
         "confirmed_at=CASE WHEN ? THEN confirmed_at ELSE ? END WHERE task_id=? AND EXISTS "
         "(SELECT 1 FROM tasks WHERE id=workflow_stages.task_id AND json_extract(data,'$.activeRun.runId')=?)",
         {int(unreachable),int(unreachable),state,int(unreachable),now,task.at("id"),task.at("activeRun").at("runId")});
}

void Store::Impl::finishStage(Json& task, const Json& report) {
    const auto id = task.at("workflowId");
    const auto stage = task.at("stageId");
    auto row = query("SELECT spec,input FROM workflow_stages WHERE workflow_id=? AND stage_id=? AND task_id=?",
                     {id,stage,task.at("id")});
    if (!row.next()) throw Conflict("Missing workflow stage assignment");
    const auto role = field(row.json(0),"role");
    const auto pin = row.string(1);
    const auto result = Json::parse(field(report,"report"),nullptr,false);
    const auto version = field(result,"artifactVersion").empty() ? field(result,"commitSha") : field(result,"artifactVersion");
    bool ok = field(report,"status")=="completed" && !report.value("truncated",false);
    std::string output;
    if (role=="verify") {
        const auto verdict = field(result,"verdict");
        ok &= version==pin && immutableVersion(pin) && (verdict=="pass" || verdict=="fail");
        if (ok) exec("INSERT INTO workflow_verdicts(workflow_id,stage_id,task_id,version,verdict,run) VALUES(?,?,?,?,?,?)",
                     {id,stage,task.at("id"),pin,verdict,report.at("run")});
        ok &= verdict=="pass";
        if (ok) output = pin;
    } else if (role=="implement" || !version.empty()) {
        ok &= immutableVersion(version);
        if (ok) {
            output = version;
            exec("INSERT INTO workflow_artifacts(workflow_id,stage_id,task_id,version,run) VALUES(?,?,?,?,?)",
                 {id,stage,task.at("id"),version,report.at("run")});
        }
    } else if (ok) output = pin;
    exec("UPDATE workflow_stages SET status=?,output=? WHERE workflow_id=? AND stage_id=?",
         {ok ? "completed" : "failed",output,id,stage});
    consumeEvent(task);
    task["state"] = "completed";
    journal(task.at("id"),"completed",{{"text",ok ? field(report,"report") : "Workflow stage failed: "+field(report,"report")},
        {"revisionId",task.at("currentRevision")},{"source",report.at("run")}});
}
}
