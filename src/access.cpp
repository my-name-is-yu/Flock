#include "storage.hpp"

#include <algorithm>

namespace flockv0 {
namespace {
bool identifier(std::string_view id) {
    return !id.empty() && id.size() <= 1024 && id.find_first_of("\r\n\t") == id.npos &&
           id.find('\0') == id.npos;
}
}

const Json& resourceOperations() {
    static const Json catalog{
        {"service",{"read","use","manage"}},{"repository",{"read","write","pr","review","issues"}},
        {"secret",{"use"}},{"session",{"read"}},{"mcp_server",{"discover","invoke","read_resource"}},
        {"environment",{"inspect","configure","launch"}},{"shared_memory",{"read","write"}},
        {"workspace",{"read","write","exec"}},{"web",{"search","fetch"}},{"bot_admin",{"create_bot","rename_bot"}}};
    return catalog;
}

bool validBotId(std::string_view id) {
    constexpr std::string_view alnum = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    return !id.empty() && id.size()<=128 && alnum.find(id.front())!=alnum.npos &&
           id.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_.")==id.npos;
}

std::string botName(std::string_view input) {
    const auto start = input.find_first_not_of(" \t\r\n");
    if (start == input.npos) throw Conflict("Bot name is required");
    const auto name = input.substr(start,input.find_last_not_of(" \t\r\n")-start+1);
    if (name.size()>128 || std::count_if(name.begin(),name.end(),[](unsigned char c){return (c & 0xC0)!=0x80;})>32 ||
        std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32 || c==127;}))
        throw Conflict("Invalid Bot name");
    return std::string(name);
}

Scope Scope::sessionRead(std::string_view environment, std::string_view thread) {
    return {"session",Json::array({environment,thread}).dump(),"read"};
}

bool Scope::valid() const {
    const auto& catalog = resourceOperations();
    const auto it = catalog.find(kind);
    if (it==catalog.end() || std::find(it->begin(),it->end(),operation)==it->end()) return false;
    if (kind=="session") {
        if (resource.size()>8192) return false;
        const auto ref = Json::parse(resource,nullptr,false);
        return ref.is_array() && ref.size()==2 && ref[0].is_string() && ref[1].is_string() &&
               identifier(ref[0].get_ref<const std::string&>()) && identifier(ref[1].get_ref<const std::string&>()) &&
               resource==ref.dump();
    }
    if (kind=="secret") return SecretRef{resource}.valid();
    if (kind=="bot_admin") return validBotId(resource);
    return identifier(resource);
}

Access Store::access() { return Access(*p_); }

void Store::Impl::initializeAccessSchema() {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS web_identity_required(id INTEGER PRIMARY KEY CHECK(id=1));
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
 json_object('requestId',NEW.id,'resourceKind',NEW.kind,'resourceId',NEW.resource,'operation',NEW.op));
END;
CREATE TABLE IF NOT EXISTS protected_inputs(id INTEGER PRIMARY KEY,bot TEXT,name TEXT,file TEXT NOT NULL UNIQUE,status TEXT DEFAULT 'pending');
CREATE UNIQUE INDEX IF NOT EXISTS pending_secret ON protected_inputs(bot,name) WHERE status='pending';
CREATE UNIQUE INDEX IF NOT EXISTS fulfilled_secret ON protected_inputs(name) WHERE status='fulfilled';
CREATE TABLE IF NOT EXISTS bot_changes(id INTEGER PRIMARY KEY,bot TEXT,operation TEXT,target TEXT,name TEXT,request_key TEXT UNIQUE NOT NULL,status TEXT DEFAULT 'pending');
CREATE TABLE IF NOT EXISTS environment_settings(bot TEXT,environment TEXT,timeout INTEGER,PRIMARY KEY(bot,environment));
)SQL";
    if (sqlite3_exec(db,schema,nullptr,nullptr,nullptr) != SQLITE_OK) throw Conflict("Cannot initialize access schema");
}

bool Access::permits(std::string_view bot, const Scope& s, bool upstream) const {
    if (!upstream || !s.valid()) return false;
    std::lock_guard lock(db_.mutex);
    if (s.kind=="session" && db_.query(R"SQL(
SELECT 1 FROM runs r JOIN tasks t ON t.id=r.task_id
 WHERE r.environment_id=json_extract(?1,'$[0]') AND r.thread_id=json_extract(?1,'$[1]') AND t.bot_id=?2 LIMIT 1
)SQL",{s.resource,bot}).next()) return true;
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
                                    const std::function<void(Id)>& execute) const {
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
        execute(task);
    } catch (...) {
        throw Conflict("Resource execution failed");
    }
    std::lock_guard lock(db_.mutex);
    db_.journal(task,"resource_used",{{"resourceKind",scope.kind},{"resourceId",scope.resource},
                                      {"operation",scope.operation},{"threadId",run.threadId},{"runId",run.runId}});
}

void Access::requireWebIdentity() {
    std::lock_guard lock(db_.mutex);
    db_.exec("INSERT OR IGNORE INTO web_identity_required VALUES(1)");
}

bool Access::requiresIdentity() const {
    std::lock_guard lock(db_.mutex);
    return db_.query("SELECT 1 FROM web_identity_required").next();
}

bool Access::permitsPerson(const Person& actor, std::string_view bot, Role role) const {
    if (!identifier(actor.id) || !validBotId(bot) || role<Role::view || role>Role::admin) return false;
    if (actor.adminRoot) return true;
    std::lock_guard lock(db_.mutex);
    return db_.query("SELECT 1 FROM person_bot_grants WHERE bot=? AND person=? AND role>=?",
                     {bot,actor.id,static_cast<int>(role)}).next();
}

void Access::personGrant(const Person& actor, std::string_view bot, std::string_view person, Role role, GrantChange change) {
    const bool revoke = change == GrantChange::revoke;
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    db_.redactor(bot).requirePublic({actor.id,bot,person});
    if (!permitsPerson(actor,bot,Role::admin) || !identifier(person) || role<Role::view || role>Role::admin)
        throw Conflict("Bot administrator required");
    if (revoke) {
        auto current = db_.query("SELECT role FROM person_bot_grants WHERE bot=? AND person=?",{bot,person});
        if (!current.next() || current.number(0) != static_cast<int>(role)) throw Conflict("Person grant has changed");
        db_.exec("DELETE FROM person_bot_grants WHERE bot=? AND person=?",{bot,person});
    }
    else db_.exec("INSERT INTO person_bot_grants VALUES(?,?,?) ON CONFLICT(bot,person) DO UPDATE SET role=excluded.role",
                  {bot,person,static_cast<int>(role)});
    requireWebIdentity();
    audit(actor,bot,revoke?"revoke_person":"grant_person",{{"person",person},{"role",static_cast<int>(role)}});
    tx.commit();
}

bool Access::permitsPersonScope(const Person& actor, const Scope& scope) const {
    std::lock_guard lock(db_.mutex);
    return identifier(actor.id) && scope.valid() && holds(actor,scope);
}

bool Access::holds(const Person& actor, const Scope& scope) const {
    return actor.adminRoot || db_.query("SELECT 1 FROM person_resource_grants WHERE person=? AND kind=? AND resource=? AND op=?",
                                        {actor.id,scope.kind,scope.resource,scope.operation}).next();
}

void Access::personScope(const Person& root, std::string_view person, const Scope& scope, GrantChange change) {
    const bool revoke = change == GrantChange::revoke;
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);

    if (!root.adminRoot || !identifier(root.id) || !identifier(person) || !scope.valid()) throw Conflict("Root capability required");
    db_.sharedRedactor().requirePublic({root.id,person,scope.resource});
    if (revoke) db_.exec("DELETE FROM person_resource_grants WHERE person=? AND kind=? AND resource=? AND op=?",
                         {person,scope.kind,scope.resource,scope.operation});
    else db_.exec("INSERT OR IGNORE INTO person_resource_grants VALUES(?,?,?,?)",{person,scope.kind,scope.resource,scope.operation});
    requireWebIdentity();
    audit(root,"",revoke?"revoke_person_scope":"grant_person_scope",
          {{"person",person},{"resourceKind",scope.kind},{"resourceId",scope.resource},{"operation",scope.operation}});
    tx.commit();
}

void Access::audit(const Person& actor, std::string_view bot, std::string_view action, const Json& scope) {
    db_.exec("INSERT INTO grant_audit_log(bot,time,actor,action,scope) VALUES(?,?,?,?,?)",
             {bot,unixTime(),actor.id,action,scope});
}

void Access::addGrant(const Person& actor, std::string_view bot, const Scope& s) {
    db_.exec("INSERT OR IGNORE INTO bot_grants VALUES(?,?,?,?,?,?)",
             {bot,s.kind,s.resource,s.operation,actor.id,unixTime()});
}

void Access::resourceGrant(const Person& actor, std::string_view bot, const Scope& s, GrantChange change) {
    const bool revoke = change == GrantChange::revoke;
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    db_.redactor(bot).requirePublic({actor.id,bot,s.resource});
    if (!s.valid() || !permitsPerson(actor,bot,Role::admin) ||
        (!revoke && !holds(actor,s))) throw Conflict("Scope exceeds grant authority");
    if (revoke) db_.exec("DELETE FROM bot_grants WHERE bot=? AND kind=? AND resource=? AND op=?",{bot,s.kind,s.resource,s.operation});
    else addGrant(actor,bot,s);
    audit(actor,bot,revoke?"revoke":"grant",{{"resourceKind",s.kind},{"resourceId",s.resource},{"operation",s.operation}});
    tx.commit();
}

Id Access::request(std::string_view bot, const Scope& s) {
    if (!validBotId(bot) || !s.valid()) throw Conflict("Invalid resource scope");
    std::lock_guard lock(db_.mutex);
    db_.redactor(bot).requirePublic({bot,s.resource});
    db_.exec("INSERT INTO grant_requests(bot,kind,resource,op,requestedBy,requestedAt) VALUES(?,?,?,?,?,?)",
             {bot,s.kind,s.resource,s.operation,bot,unixTime()});
    return db_.last();
}

void Access::decide(const Person& actor, std::string_view bot, Id id, bool approve) {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    db_.redactor(bot).requirePublic({actor.id,bot});
    auto row = db_.query("SELECT kind,resource,op FROM grant_requests WHERE id=? AND bot=? AND status='pending'",{id,bot});
    if (!row.next() || !permitsPerson(actor,bot,Role::admin)) throw Conflict("Pending request and admin access required");
    Scope scope{row.string(0),row.string(1),row.string(2)};
    if (approve && !holds(actor,scope)) throw Conflict("Scope exceeds grant authority");
    if (approve) addGrant(actor,bot,scope);
    db_.exec("UPDATE grant_requests SET status=?,decidedBy=?,decidedAt=? WHERE id=?",{approve?"approved":"denied",actor.id,unixTime(),id});
    audit(actor,bot,approve?"approve":"deny",
          {{"requestId",id},{"resourceKind",scope.kind},{"resourceId",scope.resource},{"operation",scope.operation}});
    tx.commit();
}

Json Access::grants(std::string_view bot) const {
    std::lock_guard lock(db_.mutex);
    auto rows = db_.query("SELECT kind,resource,op,approvedBy,approvedAt FROM bot_grants WHERE bot=? ORDER BY kind,resource,op",{bot});
    auto items = Json::array();
    while (rows.next()) items.push_back({{"resourceKind",rows.string(0)},{"resourceId",rows.string(1)},{"operation",rows.string(2)},
                                         {"approvedBy",rows.string(3)},{"approvedAt",rows.number(4)}});
    return db_.redactor(bot).record(items);
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
    auto safe = db_.redactor(bot).record(result);
    safe["operations"] = resourceOperations();
    return safe;
}

Id Access::requestBotChange(std::string_view bot, std::string_view operation, std::string_view target,
                            std::string_view name) {
    std::lock_guard lock(db_.mutex);
    if (!validBotId(bot) || !validBotId(target) ||
        !permits(bot,{"bot_admin",std::string(target),std::string(operation)},true))
        throw Conflict("Bot administration grant required");
    db_.redactor(bot).requirePublic({name,target});
    db_.exec("INSERT INTO bot_changes(bot,operation,target,name,request_key) VALUES(?,?,?,?,?)",
             {bot,operation,target,botName(name),actionId()});
    return db_.last();
}

void Access::reviewBotChange(const Person& actor, std::string_view bot, Id request, bool approve,
                             const ApplyBotChange& apply) {
    std::lock_guard lock(db_.mutex);
    auto authorizedChange = [&] {
        auto row = db_.query("SELECT operation,target,name,request_key,status FROM bot_changes WHERE bot=? AND id=?",
                             {bot,request});
        if (!row.next() || !permitsPerson(actor,bot,Role::admin)) throw Conflict("Bot change unavailable");
        BotChange change{row.string(0),row.string(1),row.string(2),row.string(3)};
        if (approve && (!apply || !permits(bot,{"bot_admin",change.target,change.operation},true) ||
            (change.operation=="create_bot" ? !actor.adminRoot : !permitsPerson(actor,change.target,Role::admin))))
            throw Conflict("Bot change approval denied");
        return std::pair{change,row.string(4)};
    };
    {
        Transaction tx(db_.db);
        const auto [change,status] = authorizedChange();
        if (status == "pending") {
            if (approve && change.operation=="rename_bot")
                db_.exec("UPDATE bot_changes SET status='superseded' WHERE target=? AND operation='rename_bot' AND status='approved'",
                         {change.target});
            db_.redactor(bot).requirePublic(actor.id);
            db_.exec("UPDATE bot_changes SET status=? WHERE id=?",{approve?"approved":"denied",request});
            audit(actor,bot,approve?"approve_bot_change":"deny_bot_change",
                  {{"requestId",request},{"target",change.target},{"operation",change.operation},{"name",change.name}});
        } else if (status != "approved" || !approve) throw Conflict("Bot change already decided");
        tx.commit();
    }
    if (!approve) return;
    Transaction tx(db_.db);
    const auto [change,status] = authorizedChange();
    if (status == "approved") {
        try { apply(change); } catch (const std::exception&) { throw BotChangePending{}; }
        db_.exec("UPDATE bot_changes SET status='applied' WHERE id=?",{request});
    }
    tx.commit();
}

Id Access::requestSecret(std::string_view bot, const SecretRef& ref) {
    if (!validBotId(bot) || !ref.valid()) throw Conflict("Invalid Bot or secret reference");
    std::lock_guard lock(db_.mutex);
    db_.redactor(bot).requirePublic({bot,ref.name});
    auto row = db_.query(R"SQL(
INSERT INTO protected_inputs(bot,name,file)
 SELECT ?1,?2,?3 WHERE NOT EXISTS (SELECT 1 FROM protected_inputs WHERE bot=?1 AND name=?2 AND status='fulfilled')
 ON CONFLICT(bot,name) WHERE status='pending' DO UPDATE SET status=excluded.status
 RETURNING id
)SQL",{bot,ref.name,actionId()});
    if (!row.next()) throw Conflict("Protected input unavailable");
    const auto id = row.number(0);
    row.next();
    return id;
}

void Access::submitSecret(const Person& actor, std::string_view bot, Id request, std::string_view value) {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    if (!permitsPerson(actor,bot,Role::admin)) throw Conflict("Admin required");
    auto row = db_.query(R"SQL(
SELECT file,name FROM protected_inputs p WHERE bot=? AND id=? AND status='pending'
 AND NOT EXISTS (SELECT 1 FROM protected_inputs WHERE name=p.name AND status='fulfilled')
)SQL",{bot,request});
    if (!row.next()) throw Conflict("Protected input unavailable");
    const auto name = row.string(1);
    if (!holds(actor,{"secret",name,"use"})) throw Conflict("Protected input unavailable");
    if (value.empty() || name.find(value)!=name.npos || bot.find(value)!=bot.npos) throw Conflict("Invalid protected value");
    db_.redactor(bot).requirePublic(actor.id);
    db_.secrets.insert({row.string(0)},value);
    audit(actor,bot,"supply_secret",{{"requestId",request},{"resourceKind","secret"},{"resourceId",name}});
    db_.exec("UPDATE protected_inputs SET status='fulfilled' WHERE id=?",{request});
    tx.commit();
}

void Access::useSecret(Id task, const SecretRef& ref, bool upstream, const std::function<void(std::string_view)>& adapter) const {
    std::string value;
    {
        std::lock_guard lock(db_.mutex);
        Transaction snapshot(db_.db,Transaction::Mode::read);
        if (!permitsTask(task,{"secret",ref.name,"use"},upstream)) throw Conflict("Secret access denied");
        auto row = db_.query("SELECT file FROM protected_inputs WHERE name=? AND status='fulfilled'",{ref.name});
        if (!row.next()) throw Conflict("Secret unavailable");
        value = db_.secrets.read({row.string(0)});
        snapshot.commit();
    }
    try { adapter(value); }
    catch (...) { throw Conflict("Secret adapter failed"); }
}
}
