#include "storage.hpp"

#include <algorithm>

namespace flockv0 {
namespace {
bool identifier(std::string_view id) {
    return !id.empty() && id.size() <= 1024 && id.find_first_of("\r\n\t") == id.npos &&
           id.find('\0') == id.npos;
}
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

namespace {
constexpr std::pair<Action,std::string_view> actions[] = {
    {Action::bot_view,"bot.view"},
    {Action::bot_talk,"bot.talk"},
    {Action::bot_decide,"bot.decide"},
    {Action::bot_settings,"bot.settings"},
    {Action::bot_create,"bot.create"},
    {Action::bot_rename,"bot.rename"},
    {Action::rules_manage,"rules.manage"},
    {Action::repository_read,"repository.read"},
    {Action::repository_write,"repository.write"},
    {Action::repository_pr,"repository.pr"},
    {Action::repository_review,"repository.review"},
    {Action::repository_issues,"repository.issues"},
    {Action::secret_use,"secret.use"},
    {Action::session_read,"session.read"},
    {Action::mcp_discover,"mcp.discover"},
    {Action::mcp_invoke,"mcp.invoke"},
    {Action::mcp_read_resource,"mcp.read_resource"},
    {Action::environment_inspect,"environment.inspect"},
    {Action::environment_configure,"environment.configure"},
    {Action::environment_launch,"environment.launch"},
    {Action::shared_memory_read,"shared_memory.read"},
    {Action::shared_memory_write,"shared_memory.write"},
    {Action::workspace_read,"workspace.read"},
    {Action::workspace_write,"workspace.write"},
    {Action::workspace_exec,"workspace.exec"},
    {Action::web_search,"web.search"},
    {Action::web_fetch,"web.fetch"},
    {Action::skill_list,"skill.list"},
    {Action::skill_read,"skill.read"},
    {Action::attachment_inspect,"attachment.inspect"},
    {Action::channel_use,"channel.use"},
    {Action::channel_read,"channel.read"},
    {Action::channel_post,"channel.post"},
    {Action::t3_coordinate,"t3.coordinate"},
    {Action::t3_send,"t3.send"},
    {Action::automation_change,"automation.change"},
    {Action::email_send,"email.send"},
    {Action::destructive_write,"data.destructive_write"},
};

constexpr std::string_view ruleName(RuleKind kind) {
    switch (kind) {
    case RuleKind::allow: return "allow";
    case RuleKind::ask: return "ask";
    case RuleKind::never: return "never";
    }
    throw Conflict("Unknown policy rule");
}

bool supervisedCategory(Action action) {
    switch (action) {
    case Action::repository_write:
    case Action::repository_pr:
    case Action::repository_review:
    case Action::repository_issues:
    case Action::channel_post:
    case Action::email_send:
    case Action::environment_launch:
    case Action::t3_send:
    case Action::bot_create:
    case Action::bot_rename:
    case Action::automation_change:
    case Action::destructive_write:
    case Action::secret_use:
    case Action::mcp_invoke:
        return true;
    default:
        return false;
    }
}

// Track the last wildcard and retry it with one more byte when needed.
bool targetMatches(std::string_view pattern, std::string_view target) {
    std::size_t p = 0, t = 0, star = pattern.npos, retry = 0;
    while (t < target.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            retry = t;
        } else if (p < pattern.size() && pattern[p] == target[t]) {
            ++p;
            ++t;
        } else if (star != pattern.npos) {
            p = star + 1;
            t = ++retry;
        } else return false;
    }
    return pattern.substr(p).find_first_not_of('*') == pattern.npos;
}
}

std::string_view actionName(Action action) {
    for (const auto& [value,name] : actions)
        if (value == action) return name;
    throw Conflict("Unknown policy action");
}

std::optional<Action> parseAction(std::string_view name) {
    for (const auto& [action,value] : actions)
        if (value == name) return action;
    return {};
}

std::optional<RuleKind> parseRuleKind(std::string_view name) {
    for (auto kind : {RuleKind::allow,RuleKind::ask,RuleKind::never})
        if (ruleName(kind) == name) return kind;
    return {};
}

Resource Resource::sessionRead(std::string_view environment, std::string_view thread) {
    return {Action::session_read,Json::array({environment,thread}).dump()};
}

Policy Store::policy() { return Policy(*p_); }

void Store::Impl::initializePolicySchema() {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS web_identity_required(id INTEGER PRIMARY KEY CHECK(id=1));
CREATE TABLE IF NOT EXISTS policy_settings(id INTEGER PRIMARY KEY CHECK(id=1),mode TEXT NOT NULL CHECK(mode IN ('full','supervised')), revision INTEGER NOT NULL);
INSERT OR IGNORE INTO policy_settings VALUES(1,'full',0);
CREATE TABLE IF NOT EXISTS policy_rules(id INTEGER PRIMARY KEY,kind TEXT NOT NULL CHECK(kind IN ('allow','ask','never')),
 action TEXT NOT NULL,target TEXT NOT NULL,created_at INTEGER NOT NULL,exact INTEGER NOT NULL,UNIQUE(kind,action,target));
CREATE TABLE IF NOT EXISTS policy_approvals(
 id INTEGER PRIMARY KEY, operation TEXT NOT NULL UNIQUE, bot TEXT NOT NULL,
 action TEXT NOT NULL,target TEXT NOT NULL,fingerprint TEXT NOT NULL,preview TEXT NOT NULL,
 scope TEXT NOT NULL, requester TEXT NOT NULL, anchor INTEGER NOT NULL, turn INTEGER NOT NULL,
 task INTEGER NOT NULL, revision INTEGER NOT NULL,generation INTEGER NOT NULL,
 created_at INTEGER NOT NULL,expires_at INTEGER,mode_revision INTEGER NOT NULL,
 status TEXT NOT NULL DEFAULT 'pending', resume TEXT,
 wait_id INTEGER NOT NULL DEFAULT 0, card_generation INTEGER NOT NULL, resume_state TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS policy_pending ON policy_approvals(status,bot);
CREATE INDEX IF NOT EXISTS policy_denied ON policy_approvals(bot,scope,action,target,status);
CREATE TABLE IF NOT EXISTS secret_destinations(destination TEXT PRIMARY KEY);
CREATE TABLE IF NOT EXISTS bot_owners(bot TEXT PRIMARY KEY,person TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS bot_members(bot TEXT NOT NULL,person TEXT NOT NULL,PRIMARY KEY(bot,person));
CREATE TABLE IF NOT EXISTS policy_journal(id INTEGER PRIMARY KEY,time INTEGER,actor TEXT,bot TEXT,action TEXT,details TEXT);
CREATE TABLE IF NOT EXISTS protected_inputs(id INTEGER PRIMARY KEY,bot TEXT,name TEXT,file TEXT NOT NULL UNIQUE,
 status TEXT DEFAULT 'pending',source_message_id INTEGER REFERENCES messages(id));
CREATE UNIQUE INDEX IF NOT EXISTS pending_secret ON protected_inputs(bot,name) WHERE status='pending';
CREATE UNIQUE INDEX IF NOT EXISTS fulfilled_secret ON protected_inputs(name) WHERE status='fulfilled';
CREATE TABLE IF NOT EXISTS bot_changes(id INTEGER PRIMARY KEY,bot TEXT,operation TEXT,target TEXT,name TEXT,
 request_key TEXT UNIQUE NOT NULL,source_message_id INTEGER REFERENCES messages(id));
CREATE TABLE IF NOT EXISTS environment_settings(bot TEXT,environment TEXT,timeout INTEGER,PRIMARY KEY(bot,environment));
)SQL";
    if (sqlite3_exec(db,schema,nullptr,nullptr,nullptr) != SQLITE_OK) throw Conflict("Cannot initialize access schema");
}


PolicyResult Policy::check(std::string_view bot, Action action, std::string_view target) const {
    return check(bot,Resource{action,std::string(target)});
}

bool Policy::forbidden(std::string_view bot, Action action, std::string_view target) const {
    if (!validBotId(bot) || target.empty()) return true;
    std::lock_guard lock(db_.mutex);
    auto rows = db_.query("SELECT target FROM policy_rules WHERE kind='never' AND action=?",{actionName(action)});
    while (rows.next())
        if (targetMatches(rows.string(0),target)) return true;
    return false;
}

PolicyResult Policy::check(std::string_view bot, const Resource& resource) const {
    std::lock_guard lock(db_.mutex);
    if (forbidden(bot,resource.action,resource.target)) return PolicyResult::deny;
    if (mode() == "full") return PolicyResult::allow;
    auto rows = db_.query("SELECT kind,target,exact FROM policy_rules WHERE kind<>'never' AND action=?"
                          " ORDER BY kind DESC",{actionName(resource.action)});
    while (rows.next()) {
        const auto pattern = rows.string(1);
        if (rows.number(2) ? pattern == resource.target : targetMatches(pattern,resource.target))
            return rows.string(0) == "ask" ? PolicyResult::ask : PolicyResult::allow;
    }
    return !resource.routine && supervisedCategory(resource.action) ? PolicyResult::ask : PolicyResult::allow;
}

PolicyResult Policy::check(Id task, Action action, std::string_view target) const {
    std::lock_guard lock(db_.mutex);
    auto row = db_.query("SELECT bot_id FROM tasks WHERE id=?",{task});
    return row.next() ? check(row.string(0),action,target) : PolicyResult::deny;
}

PolicyResult Policy::check(const Person& actor, Action action, std::string_view target) const {
    if (!identifier(actor.id)) return PolicyResult::deny;
    if (action == Action::bot_create) return PolicyResult::allow;
    if (action == Action::rules_manage) return actor.adminRoot ? PolicyResult::allow : PolicyResult::deny;
    if (!validBotId(target)) return PolicyResult::deny;
    std::lock_guard lock(db_.mutex);
    if (actor.adminRoot || db_.query("SELECT 1 FROM bot_owners WHERE bot=? AND person=?",{target,actor.id}).next())
        return PolicyResult::allow;
    if ((action == Action::bot_view || action == Action::bot_talk || action == Action::bot_decide) &&
        db_.query("SELECT 1 FROM bot_members WHERE bot=? AND person=?",{target,actor.id}).next())
        return PolicyResult::allow;
    return PolicyResult::deny;
}

std::string Policy::mode() const {
    std::lock_guard lock(db_.mutex);
    auto row = db_.query("SELECT mode FROM policy_settings WHERE id=1");
    if (!row.next()) throw Conflict("Installation policy unavailable");
    return row.string(0);
}

Json Policy::rules() const {
    std::lock_guard lock(db_.mutex);
    auto rows = db_.query("SELECT kind,action,target,created_at FROM policy_rules ORDER BY action,target");
    Json result = Json::array();
    while (rows.next()) result.push_back({{"kind",rows.string(0)},{"action",rows.string(1)},
                                         {"target",rows.string(2)},{"createdAt",rows.number(3)}});
    return result;
}

void Policy::setMode(const Person& actor, std::string_view value) {
    if (!allows(actor,Action::rules_manage,"installation")) throw DecisionDenied{};
    if (value != "full" && value != "supervised") throw Conflict("Unknown policy mode");
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    if (mode() != value) {
        db_.exec("UPDATE policy_settings SET mode=?,revision=revision+1 WHERE id=1",{value});
        audit(actor,"","mode_changed",{{"mode",value}});
        db_.settlePolicyApprovals();
    }
    tx.commit();
}

void Policy::changeRule(const Person& actor, RuleKind kind, Action action, std::string_view target, bool add) {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    if (!allows(actor,Action::rules_manage,"installation")) throw DecisionDenied{};
    writeRule(actor,kind,action,target,add,false);
    tx.commit();
}

void Policy::changeNeverRule(const Person& actor, Action action, std::string_view target, bool add) {
    changeRule(actor,RuleKind::never,action,target,add);
}

void Policy::writeRule(const Person& actor, RuleKind kind, Action action, std::string_view target, bool add, bool fromCard) {
    if (!identifier(target)) throw Conflict("Invalid policy rule target");
    db_.sharedRedactor().requirePublic({actor.id,target});
    const auto name = actionName(action), type = ruleName(kind);
    if (add) {
        if (db_.query("SELECT 1 FROM policy_rules WHERE kind=? AND action=? AND target=?",{type,name,target}).next())
            return;
        auto count = db_.query("SELECT count(*) FROM policy_rules WHERE kind=?",{type});
        count.next();
        if (kind != RuleKind::never && count.number(0) >= 20) {
            if (!fromCard || kind != RuleKind::allow) throw Conflict("Policy rule limit reached");
            db_.exec("DELETE FROM policy_rules WHERE id=(SELECT min(id) FROM policy_rules WHERE kind='allow')");
        }
        db_.exec("INSERT INTO policy_rules(kind,action,target,created_at,exact) VALUES(?,?,?,?,?)",
                 {type,name,target,unixTime(),fromCard});
    } else db_.exec("DELETE FROM policy_rules WHERE kind=? AND action=? AND target=?",{type,name,target});
    audit(actor,"",add ? "rule_added" : "rule_removed",{{"kind",type},{"action",name},{"target",target}});
}

void Policy::setOwner(const Person& creator, std::string_view bot) {
    std::lock_guard lock(db_.mutex);
    if (!validBotId(bot) || !allows(creator,Action::bot_create,bot)) throw DecisionDenied{};
    db_.sharedRedactor().requirePublic({creator.id,bot});
    db_.exec("INSERT INTO bot_owners VALUES(?,?)",{bot,creator.id});
}

void Policy::setMember(const Person& actor, std::string_view bot, std::string_view person, bool add) {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    if (!allows(actor,Action::bot_settings,bot) || !identifier(person)) throw DecisionDenied{};
    db_.sharedRedactor().requirePublic({actor.id,bot,person});
    if (add) db_.exec("INSERT OR IGNORE INTO bot_members VALUES(?,?)",{bot,person});
    else db_.exec("DELETE FROM bot_members WHERE bot=? AND person=?",{bot,person});
    audit(actor,bot,add ? "add_member" : "remove_member",{{"person",person}});
    tx.commit();
}

Json Policy::administration(const Person& actor, std::string_view bot) const {
    std::lock_guard lock(db_.mutex);
    if (!allows(actor,Action::bot_view,bot)) throw DecisionDenied{};
    const bool owner = allows(actor,Action::bot_settings,bot);
    Json result{{"mode",mode()},{"rules",rules()},{"canManage",owner},{"canManagePolicy",actor.adminRoot},{"members",Json::array()},
                {"secretInputs",Json::array()}};
    if (owner) {
        auto members = db_.query("SELECT person FROM bot_members WHERE bot=? ORDER BY person",{bot});
        while (members.next()) result["members"].push_back(members.string(0));
    }
    auto inputs = db_.query("SELECT id,name,status FROM protected_inputs p WHERE bot=? AND"
                            " (status='fulfilled' OR NOT EXISTS (SELECT 1 FROM protected_inputs"
                            " WHERE name=p.name AND status='fulfilled')) ORDER BY id",{bot});
    while (inputs.next()) result["secretInputs"].push_back({{"id",inputs.number(0)},{"name",inputs.string(1)},
                                                           {"status",inputs.string(2)}});
    return db_.redactor(bot).record(result);
}

void Policy::audit(const Person& actor, std::string_view bot, std::string_view action, const Json& details) {
    db_.exec("INSERT INTO policy_journal(time,actor,bot,action,details) VALUES(?,?,?,?,?)",
             {unixTime(),actor.id,bot,action,details.dump()});
}

Id Policy::applyBotChange(std::string_view bot, const Person& creator, std::string_view operation,
                          std::string_view target, std::string_view name, Id message, int step,
                          const ApplyBotChange& apply) {
    const bool create = operation == "create_bot";
    if ((!create && operation != "rename_bot") || !validBotId(target) ||
        forbidden(bot,create ? Action::bot_create : Action::bot_rename,target))
        throw Conflict("Bot change denied by policy");
    if (!apply) throw Conflict("Bot changes are unavailable");
    // A Bot acts for the person it is talking to, so renaming needs that person's authority over the target.
    if (!identifier(creator.id) || (!create && !allows(creator,Action::bot_settings,target))) throw DecisionDenied{};
    std::lock_guard lock(db_.mutex);
    db_.redactor(bot).requirePublic({name,target});
    const auto requestKey = std::to_string(message) + "-" + std::to_string(step);
    const BotChange change{std::string(operation),std::string(target),botName(name),requestKey};
    if (create && db_.query("SELECT 1 FROM bot_owners WHERE bot=?",{target}).next())
        throw Conflict("Target Bot already exists");
    apply(change);
    if (create) setOwner(creator,target);
    db_.exec("INSERT INTO bot_changes(bot,operation,target,name,request_key,source_message_id) VALUES(?,?,?,?,?,?)",
             {bot,operation,target,change.name,requestKey,message});
    return db_.last();
}

void Policy::executeSessionResource(std::string_view environment, const RunRef& run, const Resource& resource,
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
    if (!allows(task,resource.action,resource.target)) throw Conflict("Resource access denied");
    try {
        if (!upstreamPermits() || !allows(task,resource.action,resource.target)) throw Conflict("Resource access denied");
        execute(task);
    } catch (...) {
        throw Conflict("Resource execution failed");
    }
    std::lock_guard lock(db_.mutex);
    db_.journal(task,"resource_used",{{"target",resource.target},{"action",actionName(resource.action)},{"threadId",run.threadId},{"runId",run.runId}});
}

void Policy::requireWebIdentity() {
    std::lock_guard lock(db_.mutex);
    db_.exec("INSERT OR IGNORE INTO web_identity_required VALUES(1)");
}

bool Policy::requiresIdentity() const {
    std::lock_guard lock(db_.mutex);
    return db_.query("SELECT 1 FROM web_identity_required").next();
}

Id Policy::requestSecret(std::string_view bot, const SecretRef& ref, Id sourceMessage) {
    if (!validBotId(bot) || !ref.valid()) throw Conflict("Invalid Bot or secret reference");
    std::lock_guard lock(db_.mutex);
    db_.redactor(bot).requirePublic({bot,ref.name});
    auto row = db_.query(R"SQL(
INSERT INTO protected_inputs(bot,name,file,source_message_id)
 SELECT ?1,?2,?3,?4 WHERE NOT EXISTS (SELECT 1 FROM protected_inputs WHERE bot=?1 AND name=?2 AND status='fulfilled')
 ON CONFLICT(bot,name) WHERE status='pending' DO UPDATE
 SET source_message_id=coalesce(protected_inputs.source_message_id,excluded.source_message_id)
 RETURNING id
)SQL",{bot,ref.name,actionId(),sourceMessage ? Json(sourceMessage) : Json(nullptr)});
    if (!row.next()) throw Conflict("Protected input unavailable");
    const auto id = row.number(0);
    row.next();
    return id;
}

void Policy::submitSecret(const Person& actor, std::string_view bot, Id request, std::string_view value) {
    std::lock_guard lock(db_.mutex);
    Transaction tx(db_.db);
    fulfillSecret(actor,bot,request,value);
    tx.commit();
}

void Policy::fulfillSecret(const Person& actor, std::string_view bot, Id request, std::string_view value) {
    if (!allows(actor,Action::bot_decide,bot)) throw DecisionDenied{};
    auto row = db_.query(R"SQL(
SELECT file,name FROM protected_inputs p WHERE bot=? AND id=? AND status='pending'
 AND NOT EXISTS (SELECT 1 FROM protected_inputs WHERE name=p.name AND status='fulfilled')
)SQL",{bot,request});
    if (!row.next()) throw DecisionUnavailable{};
    const auto name = row.string(1);
    if (value.empty() || name.find(value)!=name.npos || bot.find(value)!=bot.npos) throw Conflict("Invalid protected value");
    db_.redactor(bot).requirePublic(actor.id);
    audit(actor,bot,"supply_secret",{{"requestId",request},{"resourceKind","secret"},{"resourceId",name}});
    db_.recordInboxDecision(actor,{std::string(bot),InboxKind::protected_input,request,0,0,0,0},"supply");
    db_.exec("UPDATE protected_inputs SET status='fulfilled' WHERE id=?",{request});
    db_.secrets.insert({row.string(0)},value);
}

void Policy::useSecret(Id task, const SecretRef& ref, std::string_view destination, std::string_view operation,
                       bool upstream, const std::function<void(std::string_view)>& adapter) const {
    std::string value;
    {
        std::lock_guard lock(db_.mutex);
        Transaction tx(db_.db);
        if (!upstream || !ref.valid() || !identifier(destination) || !identifier(operation)) throw Conflict("Secret access denied");
        const auto entry = db_.load(task);
        if (forbidden(field(entry,"botId"),Action::secret_use,ref.name)) throw Conflict("Secret access denied");
        const auto seen = db_.query("SELECT 1 FROM secret_destinations WHERE destination=?",{destination}).next();
        const auto key = "secret/" + Json::array({task,operation}).dump();
        const auto gate = db_.approveAction(db_.taskPolicyOrigin(entry),{Action::secret_use,std::string(destination),seen},
                                           key,Json{{"secret",ref.name},{"destination",destination}},db_.policyClock());
        if (gate != ApprovalGate::allow) {
            tx.commit();
            throw Conflict(gate == ApprovalGate::waiting ? "Secret use is waiting for policy approval." :
                           std::string(approvalError(gate)));
        }
        auto row = db_.query("SELECT file FROM protected_inputs WHERE name=? AND status='fulfilled'",{ref.name});
        if (!row.next()) throw Conflict("Secret unavailable");
        value = db_.secrets.read({row.string(0)});
        tx.commit();
    }
    try { adapter(value); }
    catch (...) { throw Conflict("Secret adapter failed"); }
    std::lock_guard lock(db_.mutex);
    db_.exec("INSERT OR IGNORE INTO secret_destinations VALUES(?)",{destination});
}
}
