#include "storage.hpp"

#include <map>
#include <set>

namespace flockv0 {
namespace {
const std::map<std::string,std::set<std::string>> operations{
    {"service",{"read","use","manage"}},{"repository",{"read","write","pr","review","issues"}},
    {"secret",{"use"}},{"session",{"read"}},{"mcp_server",{"discover","invoke","read_resource"}},
    {"environment",{"inspect","configure","launch"}},{"shared_memory",{"read","write"}},
    {"workspace",{"read","write","exec"}},{"web",{"search","fetch"}},{"bot_admin",{"create_bot","rename_bot"}}};

void requirePublic(const SecretStore& vault, const Json& metadata) {
    if (vault.redactSecrets(metadata) != metadata) throw Conflict("Secret values cannot be used as metadata");
}

bool identifier(std::string_view id) {
    return !id.empty() && id.size() <= 1024 && id.find_first_of("\r\n\t") == id.npos &&
           id.find('\0') == id.npos;
}
}

bool Scope::valid() const {
    const auto it = operations.find(kind);
    return it != operations.end() && it->second.contains(operation) && identifier(resource);
}

Access Store::access() { return Access(*p_); }

void Store::Impl::migrateAccess() {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS person_bot_grants(bot TEXT,person TEXT,role INTEGER,PRIMARY KEY(bot,person));
CREATE TABLE IF NOT EXISTS bot_grants(bot TEXT,kind TEXT,resource TEXT,op TEXT,approvedBy TEXT,approvedAt INTEGER,
 PRIMARY KEY(bot,kind,resource,op));
CREATE TABLE IF NOT EXISTS person_resource_grants(person TEXT,kind TEXT,resource TEXT,op TEXT,
 PRIMARY KEY(person,kind,resource,op));
CREATE TABLE IF NOT EXISTS grant_requests(id INTEGER PRIMARY KEY,bot TEXT,kind TEXT,resource TEXT,op TEXT,
 requestedBy TEXT,requestedAt INTEGER,status TEXT DEFAULT 'pending',decidedBy TEXT,decidedAt INTEGER);
CREATE TABLE IF NOT EXISTS grant_audit_log(id INTEGER PRIMARY KEY,bot TEXT,time INTEGER,actor TEXT,action TEXT,scope TEXT);
CREATE INDEX IF NOT EXISTS audit_by_bot ON grant_audit_log(bot,action);
CREATE TRIGGER IF NOT EXISTS access_requested AFTER INSERT ON grant_requests BEGIN
 INSERT INTO grant_audit_log(bot,time,actor,action,scope) VALUES(NEW.bot,NEW.requestedAt,NEW.requestedBy,'request',
 json_object('requestId',NEW.id,'kind',NEW.kind,'resource',NEW.resource,'operation',NEW.op));
END;
CREATE TABLE IF NOT EXISTS protected_inputs(id INTEGER PRIMARY KEY,bot TEXT,name TEXT,status TEXT DEFAULT 'pending');
CREATE TABLE IF NOT EXISTS bot_changes(id INTEGER PRIMARY KEY,bot TEXT,operation TEXT,target TEXT,name TEXT,status TEXT DEFAULT 'pending');
CREATE TABLE IF NOT EXISTS environment_settings(bot TEXT,environment TEXT,timeout INTEGER,PRIMARY KEY(bot,environment));
)SQL";
    if (sqlite3_exec(db,schema,nullptr,nullptr,nullptr) != SQLITE_OK) throw Conflict("Access migration failed");
}

bool Store::Impl::managedAccess(const Json& task) const {
    std::lock_guard lock(mutex);
    return task.at("config").value("enforceAccess",false) ||
           query("SELECT 1 FROM grant_audit_log WHERE bot=? AND action IN"
                 " ('grant','revoke','approve','grant_person','revoke_person') LIMIT 1",{task.at("botId")}).next();
}

bool Store::Impl::runtimePermits(const Json& task, const Scope& scope) {
    return !managedAccess(task) || Access(*this).permitsTask(task.at("id"),scope,true);
}

bool Access::permits(std::string_view bot, const Scope& s, bool upstream) const {
    if (!upstream || !s.valid()) return false;
    std::lock_guard lock(db_.mutex);
    return db_.query("SELECT 1 FROM bot_grants WHERE bot=? AND kind=? AND resource=? AND op=?",
                     {bot,s.kind,s.resource,s.operation}).next();
}

bool Access::permitsTask(Id task, const Scope& s, bool upstream) const {
    std::lock_guard lock(db_.mutex);
    auto owner = db_.query("SELECT bot_id FROM tasks WHERE id=?",{task});
    return owner.next() && permits(owner.string(0),s,upstream);
}

void Access::executeSessionResource(std::string_view environment, const RunRef& run, const Scope& scope,
                                    const std::function<bool()>& upstreamPermits,
                                    const std::function<void()>& execute) const {
    Id task;
    {
        std::lock_guard lock(db_.mutex);
        auto row = db_.query("SELECT task_id FROM runs WHERE environment_id=? AND thread_id=? AND run_id=?",
                             {environment,run.threadId,run.runId});
        if (!row.next()) throw Conflict("Unknown Session");
        task = row.number(0);
    }
    if (!permitsTask(task,scope,true)) throw Conflict("Resource access denied");
    try {
        if (!permitsTask(task,scope,upstreamPermits())) throw Conflict("Resource access denied");
        execute();
    } catch (const std::exception& error) {
        throw Conflict(db_.secrets.redactSecrets(std::string(error.what())).get<std::string>());
    }
    std::lock_guard lock(db_.mutex);
    db_.journal(task,"resource_used",{{"resourceKind",scope.kind},{"resourceId",scope.resource},
                                      {"operation",scope.operation},{"threadId",run.threadId},{"runId",run.runId}});
}

bool Access::requiresIdentity() const {
    std::lock_guard lock(db_.mutex);
    return db_.query("SELECT 1 FROM grant_audit_log WHERE action IN ('grant_person','grant_person_scope') LIMIT 1").next();
}

bool Access::permitsPerson(const Person& actor, std::string_view bot, Role role) const {
    if (!identifier(actor.id) || !identifier(bot)) return false;
    if (actor.adminRoot) return true;
    std::lock_guard lock(db_.mutex);
    return db_.query("SELECT 1 FROM person_bot_grants WHERE bot=? AND person=? AND role>=?",
                     {bot,actor.id,static_cast<int>(role)}).next();
}

void Access::personGrant(const Person& actor, std::string_view bot, std::string_view person, Role role, bool revoke) {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    requirePublic(db_.secrets,{actor.id,bot,person});
    if (!permitsPerson(actor,bot,Role::admin) || !identifier(person) || role<Role::view || role>Role::admin)
        throw Conflict("Bot administrator required");
    if (revoke) {
        auto current = db_.query("SELECT role FROM person_bot_grants WHERE bot=? AND person=?",{bot,person});
        if (!current.next() || current.number(0) != static_cast<int>(role)) throw Conflict("Person grant has changed");
        db_.exec("DELETE FROM person_bot_grants WHERE bot=? AND person=?",{bot,person});
    }
    else db_.exec("INSERT INTO person_bot_grants VALUES(?,?,?) ON CONFLICT(bot,person) DO UPDATE SET role=excluded.role",
                  {bot,person,static_cast<int>(role)});
    db_.exec("INSERT INTO grant_audit_log(bot,time,actor,action,scope) VALUES(?,?,?,?,?)",
             {bot,unixTime(),actor.id,revoke?"revoke_person":"grant_person",Json{{"person",person},{"role",static_cast<int>(role)}}});
    tx.commit();
}

bool Access::holds(const Person& actor, const Scope& scope) const {
    return actor.adminRoot || db_.query("SELECT 1 FROM person_resource_grants WHERE person=? AND kind=? AND resource=? AND op=?",
                                        {actor.id,scope.kind,scope.resource,scope.operation}).next();
}

void Access::personScope(const Person& root, std::string_view person, const Scope& scope, bool revoke) {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    requirePublic(db_.secrets,{root.id,person,scope.resource});
    if (!root.adminRoot || !identifier(root.id) || !identifier(person) || !scope.valid()) throw Conflict("Root capability required");
    if (revoke) db_.exec("DELETE FROM person_resource_grants WHERE person=? AND kind=? AND resource=? AND op=?",
                         {person,scope.kind,scope.resource,scope.operation});
    else db_.exec("INSERT OR IGNORE INTO person_resource_grants VALUES(?,?,?,?)",{person,scope.kind,scope.resource,scope.operation});
    db_.exec("INSERT INTO grant_audit_log(bot,time,actor,action,scope) VALUES('',?,?,?,?)",
             {unixTime(),root.id,revoke?"revoke_person_scope":"grant_person_scope",Json{{"person",person},{"kind",scope.kind},{"resource",scope.resource},{"operation",scope.operation}}});
    tx.commit();
}

void Access::resourceGrant(const Person& actor, std::string_view bot, const Scope& s, bool revoke) {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    requirePublic(db_.secrets,{actor.id,bot,s.resource});
    if (!s.valid() || !permitsPerson(actor,bot,Role::admin) ||
        (!revoke && !holds(actor,s))) throw Conflict("Scope exceeds grant authority");
    if (revoke) db_.exec("DELETE FROM bot_grants WHERE bot=? AND kind=? AND resource=? AND op=?",{bot,s.kind,s.resource,s.operation});
    else db_.exec("INSERT OR IGNORE INTO bot_grants VALUES(?,?,?,?,?,?)",{bot,s.kind,s.resource,s.operation,actor.id,unixTime()});
    db_.exec("INSERT INTO grant_audit_log(bot,time,actor,action,scope) VALUES(?,?,?,?,?)",
             {bot,unixTime(),actor.id,revoke?"revoke":"grant",Json{{"kind",s.kind},{"resource",s.resource},{"operation",s.operation}}});
    tx.commit();
}

Id Access::request(std::string_view bot, const Scope& s) {
    if (!identifier(bot) || !s.valid()) throw Conflict("Invalid resource scope");
    std::lock_guard lock(db_.mutex);
    requirePublic(db_.secrets,{bot,s.resource});
    db_.exec("INSERT INTO grant_requests(bot,kind,resource,op,requestedBy,requestedAt) VALUES(?,?,?,?,?,?)",
             {bot,s.kind,s.resource,s.operation,bot,unixTime()});
    return db_.last();
}

void Access::decide(const Person& actor, std::string_view bot, Id id, bool approve) {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    requirePublic(db_.secrets,{actor.id,bot});
    auto row = db_.query("SELECT kind,resource,op FROM grant_requests WHERE id=? AND bot=? AND status='pending'",{id,bot});
    if (!row.next() || !permitsPerson(actor,bot,Role::admin)) throw Conflict("Pending request and admin access required");
    Scope scope{row.string(0),row.string(1),row.string(2)};
    if (approve && !holds(actor,scope)) throw Conflict("Scope exceeds grant authority");
    if (approve) db_.exec("INSERT OR IGNORE INTO bot_grants VALUES(?,?,?,?,?,?)",{bot,scope.kind,scope.resource,scope.operation,actor.id,unixTime()});
    db_.exec("UPDATE grant_requests SET status=?,decidedBy=?,decidedAt=? WHERE id=?",{approve?"approved":"denied",actor.id,unixTime(),id});
    db_.exec("INSERT INTO grant_audit_log(bot,time,actor,action,scope) VALUES(?,?,?,?,?)",
             {bot,unixTime(),actor.id,approve?"approve":"deny",Json{{"requestId",id},{"kind",scope.kind},{"resource",scope.resource},{"operation",scope.operation}}});
    tx.commit();
}

Json Access::grants(std::string_view bot) const {
    std::lock_guard lock(db_.mutex);
    auto rows = db_.query("SELECT kind,resource,op,approvedBy,approvedAt FROM bot_grants WHERE bot=? ORDER BY kind,resource,op",{bot});
    auto items = Json::array();
    while (rows.next()) items.push_back({{"resourceKind",rows.string(0)},{"resourceId",rows.string(1)},{"operation",rows.string(2)},
                                         {"approvedBy",rows.string(3)},{"approvedAt",rows.number(4)}});
    return db_.secrets.redactSecrets(items);
}

Json Access::administration(const Person& actor, std::string_view bot) const {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db,Transaction::Mode::read);
    if (!permitsPerson(actor,bot,Role::admin)) throw Conflict("Admin required");
    Json result{{"grants",grants(bot)}};
    const auto list = [&](const char* key, const char* sql) {
        auto rows = db_.query(sql,{bot});
        auto& items = result[key] = Json::array();
        while (rows.next()) items.push_back(rows.json(0));
    };
    list("people","SELECT json_object('person',person,'role',CASE role WHEN 3 THEN 'admin' WHEN 2 THEN 'decide' ELSE 'view' END) FROM person_bot_grants WHERE bot=?");
    list("requests","SELECT json_object('id',id,'resourceKind',kind,'resourceId',resource,'operation',op,'status',status,'requestedBy',requestedBy,'requestedAt',requestedAt,'decidedBy',decidedBy,'decidedAt',decidedAt) FROM grant_requests WHERE bot=? ORDER BY id");
    list("audit","SELECT json_object('id',id,'time',time,'actor',actor,'action',action,'scope',json(scope)) FROM grant_audit_log WHERE bot=? ORDER BY id");
    list("secretInputs","SELECT json_object('id',id,'name',name,'status',status) FROM protected_inputs WHERE bot=?");
    list("botChanges","SELECT json_object('id',id,'operation',operation,'target',target,'name',name,'status',status) FROM bot_changes WHERE bot=?");
    tx.commit();
    return db_.secrets.redactSecrets(result);
}

void Access::reviewBotChange(const Person& actor, std::string_view bot, Id request, bool approve) {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    auto row = db_.query("SELECT target,operation FROM bot_changes WHERE bot=? AND id=? AND status='pending'",{bot,request});
    if (!row.next() || !permitsPerson(actor,bot,Role::admin) ||
        (approve && (!permits(bot,{"bot_admin",row.string(0),row.string(1)},true) ||
                    !permitsPerson(actor,row.string(0),Role::admin)))) throw Conflict("Change approval denied");
    db_.exec("UPDATE bot_changes SET status=? WHERE id=?",{approve?"approved":"denied",request});
    db_.exec("INSERT INTO grant_audit_log(bot,time,actor,action,scope) VALUES(?,?,?,?,?)",
             {bot,unixTime(),actor.id,approve?"approve_bot_change":"deny_bot_change",Json{{"requestId",request}}});
    tx.commit();
}

Id Access::requestSecret(std::string_view bot, const SecretRef& ref) {
    if (!identifier(bot) || !ref.valid()) throw Conflict("Invalid Bot or secret reference");
    std::lock_guard lock(db_.mutex);
    requirePublic(db_.secrets,{bot,ref.name});
    db_.exec("INSERT INTO protected_inputs(bot,name) VALUES(?,?)",{bot,ref.name});
    return db_.last();
}

void Access::submitSecret(const Person& actor, std::string_view bot, Id request, std::string_view value) {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    if (!permitsPerson(actor,bot,Role::admin)) throw Conflict("Admin required");
    auto row = db_.query("SELECT name FROM protected_inputs WHERE bot=? AND id=? AND status='pending'",{bot,request});
    if (!row.next()) throw Conflict("Pending input required");
    db_.secrets.insert({row.string(0)},value);
    db_.exec("UPDATE protected_inputs SET status='fulfilled' WHERE id=?",{request});
    tx.commit();
}

void Access::useSecret(Id task, const SecretRef& ref, bool upstream, const std::function<void(std::string_view)>& adapter) const {
    if (!permitsTask(task,{"secret",ref.name,"use"},upstream)) throw Conflict("Secret access denied");
    auto value = db_.secrets.read(ref);
    try { adapter(value); }
    catch (...) { throw Conflict("Secret adapter failed"); }
}
}
