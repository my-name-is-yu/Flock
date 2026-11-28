#include "flock/policy.hpp"
#include "flock/board_inbox.hpp"
#include "flock/bot_access.hpp"
#include "flock/bot_id.hpp"
#include "flock/decision_errors.hpp"
#include "flock/person.hpp"
#include "flock/policy_rules.hpp"
#include "flock/run_ref.hpp"
#include "flock/secret_ref.hpp"
#include "flock/secret_requests.hpp"
#include "flock/secrets.hpp"
#include "flock/task.hpp"
#include "policy_internal.hpp"
#include "store_access.hpp"

#include <cstddef>
#include <utility>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace flockv0 {
namespace {
constexpr std::pair<Action, std::string_view> actions[] = {
    {Action::bot_view, "bot.view"},
    {Action::bot_talk, "bot.talk"},
    {Action::bot_decide, "bot.decide"},
    {Action::bot_settings, "bot.settings"},
    {Action::bot_create, "bot.create"},
    {Action::rules_manage, "rules.manage"},
    {Action::repository_read, "repository.read"},
    {Action::repository_write, "repository.write"},
    {Action::repository_pr, "repository.pr"},
    {Action::repository_review, "repository.review"},
    {Action::repository_issues, "repository.issues"},
    {Action::secret_use, "secret.use"},
    {Action::session_read, "session.read"},
    {Action::mcp_discover, "mcp.discover"},
    {Action::mcp_invoke, "mcp.invoke"},
    {Action::mcp_read_resource, "mcp.read_resource"},
    {Action::environment_inspect, "environment.inspect"},
    {Action::environment_launch, "environment.launch"},
    {Action::shared_memory_read, "shared_memory.read"},
    {Action::shared_memory_write, "shared_memory.write"},
    {Action::private_memory_write, "memory.write"},
    {Action::workspace_read, "workspace.read"},
    {Action::workspace_write, "workspace.write"},
    {Action::web_search, "web.search"},
    {Action::web_fetch, "web.fetch"},
    {Action::skill_list, "skill.list"},
    {Action::skill_read, "skill.read"},
    {Action::attachment_inspect, "attachment.inspect"},
    {Action::channel_use, "channel.use"},
    {Action::channel_read, "channel.read"},
    {Action::channel_post, "channel.post"},
    {Action::t3_send, "t3.send"},
    {Action::automation_change, "automation.change"},
    {Action::email_send, "email.send"},
    {Action::destructive_write, "data.destructive_write"},
};

bool validIdentifier(std::string_view id) {
    return !id.empty() && id.size() <= 1024 && id.find_first_of("\r\n\t") == id.npos && id.find('\0') == id.npos;
}

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
    case Action::bot_settings:
    case Action::automation_change:
    case Action::destructive_write:
    case Action::secret_use:
    case Action::mcp_invoke: return true;
    default: return false;
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
        } else
            return false;
    }
    return pattern.substr(p).find_first_not_of('*') == pattern.npos;
}
}

std::string_view actionName(Action action) {
    for (const auto& [value, name] : actions)
        if (value == action) return name;
    throw Conflict("Unknown policy action");
}

std::optional<Action> parseAction(std::string_view name) {
    for (const auto& [action, value] : actions)
        if (value == name) return action;
    return {};
}

std::optional<RuleKind> parseRuleKind(std::string_view name) {
    for (auto kind : {RuleKind::allow, RuleKind::ask, RuleKind::never})
        if (ruleName(kind) == name) return kind;
    return {};
}

Resource Resource::sessionRead(std::string_view environment, std::string_view thread) {
    return {Action::session_read, Json::array({environment, thread}).dump()};
}

SecretRedactor Policy::secretRedactor(std::string_view bot) const {
    return StoreAccess::get(store_).secretStorage.redactor(bot);
}

Json Policy::redactSecrets(std::string_view bot, const Json& data) const {
    return secretRedactor(bot).record(data, SecretRedactor::Fields::external);
}

PolicyResult Policy::check(std::string_view bot, Action action, std::string_view target) const {
    return check(bot, Resource{action, std::string(target)});
}

bool Policy::forbidden(std::string_view bot, Action action, std::string_view target) const {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    if (!validBotId(bot) || target.empty()) return true;
    std::lock_guard lock(database.mutex);
    if (storage.botLifecycle.botDeleted(bot)) return true;
    auto rows = database.query("SELECT target FROM policy_rules WHERE kind='never' AND action=?", {actionName(action)});
    while (rows.next())
        if (targetMatches(rows.string(0), target)) return true;
    return false;
}

PolicyResult Policy::check(std::string_view bot, const Resource& resource) const {
    auto& storage = StoreAccess::get(store_);
    std::lock_guard lock(storage.database.mutex);
    if (forbidden(bot, resource.action, resource.target)) return PolicyResult::deny;
    if (mode() == "full") return PolicyResult::allow;
    auto rows = storage.database.query("SELECT kind,target,exact FROM policy_rules WHERE kind<>'never' AND action=?"
                                       " ORDER BY kind DESC",
                                       {actionName(resource.action)});
    while (rows.next()) {
        const auto pattern = rows.string(1);
        if (rows.number(2) ? pattern == resource.target : targetMatches(pattern, resource.target))
            return rows.string(0) == "ask" ? PolicyResult::ask : PolicyResult::allow;
    }
    return !resource.routine && supervisedCategory(resource.action) ? PolicyResult::ask : PolicyResult::allow;
}

PolicyResult Policy::check(Id task, Action action, std::string_view target) const {
    auto& storage = StoreAccess::get(store_);
    std::lock_guard lock(storage.database.mutex);
    auto row = storage.database.query("SELECT bot_id FROM tasks WHERE id=?", {task});
    return row.next() && !storage.botLifecycle.botPaused(row.string(0)) ? check(row.string(0), action, target) : PolicyResult::deny;
}

PolicyResult Policy::check(const Person& actor, Action action, std::string_view target) const {
    auto& storage = StoreAccess::get(store_);
    if (!validIdentifier(actor.id)) return PolicyResult::deny;
    if (action == Action::bot_create) return PolicyResult::allow;
    if (action == Action::rules_manage || action == Action::shared_memory_read || action == Action::shared_memory_write)
        return actor.adminRoot ? PolicyResult::allow : PolicyResult::deny;
    if (!validBotId(target)) return PolicyResult::deny;
    std::lock_guard lock(storage.database.mutex);
    if (storage.botLifecycle.botDeleted(target)) return PolicyResult::deny;
    if (actor.adminRoot || storage.database.query("SELECT 1 FROM bot_owners WHERE bot=? AND person=?", {target, actor.id}).next())
        return PolicyResult::allow;
    if ((action == Action::bot_view || action == Action::bot_talk || action == Action::bot_decide) &&
        storage.database.query("SELECT 1 FROM bot_members WHERE bot=? AND person=?", {target, actor.id}).next())
        return PolicyResult::allow;
    return PolicyResult::deny;
}

std::string Policy::mode() const {
    auto& database = StoreAccess::get(store_).database;
    std::lock_guard lock(database.mutex);
    auto row = database.query("SELECT mode FROM policy_settings WHERE id=1");
    if (!row.next()) throw Conflict("Installation policy unavailable");
    return row.string(0);
}

void Policy::executeSessionResource(std::string_view environment, const RunRef& run, const Resource& resource,
                                    const std::function<bool()>& upstreamPermits, const std::function<void(Id)>& execute) const {
    auto& database = StoreAccess::get(store_).database;
    Id task;
    {
        std::lock_guard lock(database.mutex);
        auto row = database.query("SELECT task_id FROM runs WHERE environment_id=? AND thread_id=? AND run_id=?",
                                  {environment, run.threadId, run.runId});
        if (!row.next()) throw Conflict("Unknown Session");
        task = row.number(0);
    }
    if (!allows(task, resource.action, resource.target)) throw Conflict("Resource access denied");
    try {
        if (!upstreamPermits() || !allows(task, resource.action, resource.target)) throw Conflict("Resource access denied");
        execute(task);
    } catch (...) {
        throw Conflict("Resource execution failed");
    }
    std::lock_guard lock(database.mutex);
    database.writeJournal(
        task, JournalKind::resource_used,
        {{"target", resource.target}, {"action", actionName(resource.action)}, {"threadId", run.threadId}, {"runId", run.runId}});
}

bool Policy::requiresIdentity() const {
    auto& database = StoreAccess::get(store_).database;
    std::lock_guard lock(database.mutex);
    return database.query("SELECT 1 FROM web_identity_required").next();
}

Json PolicyRules::rules() const {
    auto& database = StoreAccess::get(store_).database;
    std::lock_guard lock(database.mutex);
    auto rows = database.query("SELECT kind,action,target,created_at FROM policy_rules ORDER BY action,target");
    Json result = Json::array();
    while (rows.next())
        result.push_back({{"kind", rows.string(0)}, {"action", rows.string(1)}, {"target", rows.string(2)}, {"createdAt", rows.number(3)}});
    return result;
}

void PolicyRules::setMode(const Person& actor, std::string_view value) {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    if (!storage.permissions.allows(actor, Action::rules_manage, "installation")) throw AccessDenied{};
    if (value != "full" && value != "supervised") throw Conflict("Unknown policy mode");
    std::lock_guard lock(database.mutex);
    Transaction tx(database.db);
    if (storage.permissions.mode() != value) {
        database.exec("UPDATE policy_settings SET mode=?,revision=revision+1 WHERE id=1", {value});
        policy_storage::audit(database, actor, "", "mode_changed", {{"mode", value}});
        storage.policyApprovals.settlePolicyApprovals();
    }
    tx.commit();
}

void PolicyRules::changeRule(const Person& actor, RuleKind kind, Action action, std::string_view target, bool add) {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    std::lock_guard lock(database.mutex);
    Transaction tx(database.db);
    if (!storage.permissions.allows(actor, Action::rules_manage, "installation")) throw AccessDenied{};
    policy_storage::writeRule(database, storage.secretStorage, actor, kind, action, target, add, false);
    tx.commit();
}

void PolicyRules::changeNeverRule(const Person& actor, Action action, std::string_view target, bool add) {
    changeRule(actor, RuleKind::never, action, target, add);
}

void PolicyRules::requireWebIdentity() {
    auto& database = StoreAccess::get(store_).database;
    std::lock_guard lock(database.mutex);
    database.exec("INSERT OR IGNORE INTO web_identity_required VALUES(1)");
}

void policy_storage::writeRule(Database& database, const SecretStorage& secretStorage, const Person& actor, RuleKind kind, Action action,
                               std::string_view target, bool add, bool exact) {
    if (!validIdentifier(target)) throw Conflict("Invalid policy rule target");
    secretStorage.sharedRedactor().requirePublic({actor.id, target});
    const auto name = actionName(action), type = ruleName(kind);
    if (add) {
        database.exec("INSERT INTO policy_rules(kind,action,target,created_at,exact) VALUES(?,?,?,?,?)"
                      " ON CONFLICT(kind,action,target) DO NOTHING",
                      {type, name, target, unixTime(), exact});
        if (!sqlite3_changes(database.db)) return;
    } else
        database.exec("DELETE FROM policy_rules WHERE kind=? AND action=? AND target=?", {type, name, target});
    policy_storage::audit(database, actor, "", add ? "rule_added" : "rule_removed", {{"kind", type}, {"action", name}, {"target", target}});
}

Json BotAccess::createOwnedBot(const Person& creator, const std::function<Json(const PublishOwnedBot&)>& create) {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    std::lock_guard lock(database.mutex);
    if (!storage.permissions.allows(creator, Action::bot_create, "")) throw AccessDenied{};
    storage.secretStorage.sharedRedactor().requirePublic({creator.id});
    return create([&](std::string_view bot, const std::function<void()>& publish) {
        Transaction ownership(database.db);
        setOwner(creator, bot);
        publish();
        ownership.commit();
    });
}

void BotAccess::rememberProjectAssignment(const Person& person, std::string_view bot, std::string_view environment,
                                          std::string_view project) {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    std::lock_guard lock(database.mutex);
    if (sqlite3_get_autocommit(database.db)) throw Conflict("Project assignment requires a write transaction");
    storage.secretStorage.redactor(bot).requirePublic({person.id, environment, project});
    database.exec(R"SQL(
INSERT INTO recent_project_assignments(person,environment_id,project_id) VALUES(?,?,?)
ON CONFLICT(person) DO UPDATE SET environment_id=excluded.environment_id,project_id=excluded.project_id
)SQL",
                  {person.id, environment, project});
}

Json BotAccess::recentProjectAssignment(const Person& person) const {
    auto& database = Database::of(store_);
    std::lock_guard lock(database.mutex);
    auto row = database.query("SELECT environment_id,project_id FROM recent_project_assignments WHERE person=?", {person.id});
    if (!row.next()) return nullptr;
    return {{"environmentId", row.string(0)}, {"projectId", row.string(1)}};
}

void BotAccess::setOwner(const Person& creator, std::string_view bot) {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    std::lock_guard lock(database.mutex);
    if (!validBotId(bot) || !storage.permissions.allows(creator, Action::bot_create, bot)) throw AccessDenied{};
    if (storage.botLifecycle.botDeleted(bot)) throw Conflict("This Bot ID has already been used");
    storage.secretStorage.sharedRedactor().requirePublic({creator.id, bot});
    if (database.query("SELECT 1 FROM bot_creation_intents WHERE target=?", {bot}).next())
        throw Conflict("Bot creation is already in progress");
    database.exec("INSERT INTO bot_owners VALUES(?,?)", {bot, creator.id});
}

std::vector<std::string> BotAccess::peerIds(std::string_view bot) const {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    std::lock_guard lock(database.mutex);
    auto rows = database.query(R"SQL(
SELECT peer.bot FROM bot_owners sender JOIN bot_owners peer ON peer.person=sender.person
WHERE sender.bot=? AND peer.bot<>sender.bot ORDER BY peer.bot
)SQL",
                               {bot});
    std::vector<std::string> peers;
    while (rows.next()) {
        auto id = rows.string(0);
        if (!storage.botLifecycle.botDeleted(id) && !storage.botLifecycle.botPaused(id)) peers.push_back(std::move(id));
    }
    return peers;
}

void BotAccess::setMember(const Person& actor, std::string_view bot, std::string_view person, bool add) {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    std::lock_guard lock(database.mutex);
    Transaction tx(database.db);
    if (!storage.permissions.allows(actor, Action::bot_settings, bot) || !validIdentifier(person)) throw AccessDenied{};
    storage.secretStorage.sharedRedactor().requirePublic({actor.id, bot, person});
    if (add)
        database.exec("INSERT OR IGNORE INTO bot_members VALUES(?,?)", {bot, person});
    else
        database.exec("DELETE FROM bot_members WHERE bot=? AND person=?", {bot, person});
    policy_storage::audit(database, actor, bot, add ? "add_member" : "remove_member", {{"person", person}});
    tx.commit();
}

Json BotAccess::botSettings(const Person& actor, std::string_view bot) const {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    std::lock_guard lock(database.mutex);
    if (!storage.permissions.allows(actor, Action::bot_view, bot)) throw AccessDenied{};
    const bool owner = storage.permissions.allows(actor, Action::bot_settings, bot);
    Json result{
        {"canManage", owner}, {"paused", storage.botLifecycle.botPaused(bot)}, {"members", Json::array()}, {"secretInputs", Json::array()}};
    if (owner) {
        auto members = database.query("SELECT person FROM bot_members WHERE bot=? ORDER BY person", {bot});
        while (members.next()) result["members"].push_back(members.string(0));
    }
    auto inputs = database.query("SELECT id,name,status FROM protected_inputs p WHERE bot=? AND"
                                 " (status='fulfilled' OR NOT EXISTS (SELECT 1 FROM protected_inputs"
                                 " WHERE name=p.name AND status='fulfilled')) ORDER BY id",
                                 {bot});
    while (inputs.next())
        result["secretInputs"].push_back({{"id", inputs.number(0)}, {"name", inputs.string(1)}, {"status", inputs.string(2)}});
    return storage.secretStorage.redactor(bot).record(result);
}

void policy_storage::audit(Database& database, const Person& actor, std::string_view bot, std::string_view action, const Json& details) {
    database.exec("INSERT INTO policy_journal(time,actor,bot,action,details) VALUES(?,?,?,?,?)",
                  {unixTime(), actor.id, bot, action, details.dump()});
}

Id BotAccess::applyBotChange(std::string_view bot, const Person& creator, std::string_view target, std::string_view name,
                             std::optional<BotAvatarChoice> avatar, Id message, int step, const ApplyBotChange& apply,
                             const ReconcileBotCreation* onExhausted) {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    const auto requestKey = std::to_string(message) + "-" + std::to_string(step);
    if (!apply) throw Conflict("Bot creation is unavailable");
    const BotChange change{std::string(target), botName(name), requestKey, std::move(avatar)};
    std::lock_guard lock(database.mutex);
    if (auto completed = database.query("SELECT id FROM bot_creations WHERE request_key=?", {requestKey}); completed.next())
        return completed.number(0);
    if (storage.botLifecycle.botDeleted(target)) throw Conflict("This Bot ID has already been used");

    std::string owner;
    auto pending = database.query("SELECT person FROM bot_creation_intents WHERE request_key=? AND target=?", {requestKey, target});
    if (pending.next()) owner = pending.string(0);
    if (owner.empty()) {
        if (!validBotId(target) || storage.permissions.forbidden(bot, Action::bot_create, target))
            throw Conflict("Bot creation denied by policy");
        if (!validIdentifier(creator.id)) throw AccessDenied{};
        storage.secretStorage.redactor(bot).requirePublic({name, target});
        Transaction intent(database.db);
        storage.secretStorage.sharedRedactor().requirePublic({creator.id, target});
        if (database
                .query("SELECT 1 FROM bot_owners WHERE bot=? UNION ALL SELECT 1 FROM bot_creation_intents WHERE target=?", {target, target})
                .next())
            throw Conflict("Target Bot already exists");
        owner = creator.id;
        database.exec("INSERT INTO bot_creation_intents VALUES(?,?,?)", {requestKey, target, owner});
        intent.commit();
    }

    Transaction completion(database.db);
    if (auto completed = database.query("SELECT id FROM bot_creations WHERE request_key=?", {requestKey}); completed.next())
        return completed.number(0);
    try {
        apply(change);
    } catch (const Conflict&) {
        database.exec("DELETE FROM bot_creation_intents WHERE request_key=?", {requestKey});
        completion.commit();
        throw;
    } catch (...) {
        if (!onExhausted) throw BotCreationUncertain{};
        bool published = false;
        try {
            published = *onExhausted && (*onExhausted)(change);
        } catch (...) {
            throw BotCreationUncertain{};
        }
        if (!published) {
            database.exec("DELETE FROM bot_creation_intents WHERE request_key=?", {requestKey});
            completion.commit();
            throw Conflict("Unable to create Bot");
        }
    }

    database.exec("INSERT INTO bot_owners VALUES(?,?)", {target, owner});
    database.exec("DELETE FROM bot_creation_intents WHERE request_key=?", {requestKey});
    database.exec("INSERT INTO bot_creations(bot,target,name,request_key,source_message_id) VALUES(?,?,?,?,?)",
                  {bot, target, change.name, requestKey, message});
    const auto id = database.last();
    completion.commit();
    return id;
}

Id SecretRequests::requestSecret(std::string_view bot, const SecretRef& ref, Id sourceMessage) {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    if (!validBotId(bot) || !ref.valid()) throw Conflict("Invalid Bot or secret reference");
    std::lock_guard lock(database.mutex);
    storage.secretStorage.redactor(bot).requirePublic({bot, ref.name});
    auto row = database.query(R"SQL(
INSERT INTO protected_inputs(bot,name,file,source_message_id)
 SELECT ?1,?2,?3,?4 WHERE NOT EXISTS (SELECT 1 FROM protected_inputs WHERE name=?2 AND status='fulfilled')
 ON CONFLICT(bot,name) WHERE status='pending' DO UPDATE
 SET source_message_id=coalesce(protected_inputs.source_message_id,excluded.source_message_id)
 RETURNING id
)SQL",
                              {bot, ref.name, actionId(), sourceMessage ? Json(sourceMessage) : Json(nullptr)});
    if (!row.next()) throw Conflict("Protected input unavailable");
    const auto id = row.number(0);
    row.next();
    return id;
}

void SecretRequests::submitSecret(const Person& actor, std::string_view bot, Id request, std::string_view value) {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    std::lock_guard lock(database.mutex);
    Transaction tx(database.db);
    policy_storage::fulfillSecret(database, storage.permissions, storage.secretStorage, storage.inboxDecisions, actor, bot, request, value);
    tx.commit();
}

void policy_storage::fulfillSecret(Database& database, const Policy& permissions, SecretStorage& secretStorage,
                                   InboxDecisions& inboxDecisions, const Person& actor, std::string_view bot, Id request,
                                   std::string_view value) {
    if (!permissions.allows(actor, Action::bot_decide, bot)) throw AccessDenied{};
    auto row = database.query(R"SQL(
SELECT file,name FROM protected_inputs p WHERE bot=? AND id=? AND status='pending'
 AND NOT EXISTS (SELECT 1 FROM protected_inputs WHERE name=p.name AND status='fulfilled')
)SQL",
                              {bot, request});
    if (!row.next()) throw DecisionUnavailable{};
    const auto name = row.string(1);
    if (value.empty() || name.find(value) != name.npos || bot.find(value) != bot.npos) throw Conflict("Invalid protected value");
    secretStorage.redactor(bot).requirePublic(actor.id);
    policy_storage::audit(database, actor, bot, "supply_secret",
                          {{"requestId", request}, {"resourceKind", "secret"}, {"resourceId", name}});
    inboxDecisions.record(actor, {std::string(bot), InboxKind::protected_input, request, 0, 0, 0, 0}, "supply");
    database.exec("UPDATE protected_inputs SET status='fulfilled' WHERE id=?", {request});
    secretStorage.insert({row.string(0)}, value);
}

void SecretRequests::useSecret(Id task, const SecretRef& ref, std::string_view destination, std::string_view operation, bool upstream,
                               const std::function<void(std::string_view)>& adapter) const {
    auto& storage = StoreAccess::get(store_);
    auto& database = storage.database;
    std::string value;
    {
        std::lock_guard lock(database.mutex);
        Transaction tx(database.db);
        if (!upstream || !ref.valid() || !validIdentifier(destination) || !validIdentifier(operation))
            throw Conflict("Secret access denied");
        const auto entry = storage.taskRecords.load(task);
        const auto& bot = entry.botId;
        if (storage.botLifecycle.botPaused(bot) || storage.permissions.forbidden(bot, Action::secret_use, ref.name))
            throw Conflict("Secret access denied");
        const auto seen = database.query("SELECT 1 FROM secret_destinations WHERE destination=?", {destination}).next();
        const auto key = "secret/" + Json::array({task, operation}).dump();
        const auto gate = storage.policyApprovals.approveAction(
            storage.policyApprovals.taskPolicyOrigin(entry), {Action::secret_use, std::string(destination), seen}, key,
            Json{{"secret", ref.name}, {"destination", destination}}, storage.policyApprovals.now());
        if (gate != ApprovalGate::allow) {
            tx.commit();
            throw Conflict(gate == ApprovalGate::waiting ? "Secret use is waiting for policy approval." : std::string(approvalError(gate)));
        }
        auto row = database.query("SELECT file FROM protected_inputs WHERE name=? AND status='fulfilled'", {ref.name});
        if (!row.next()) throw Conflict("Secret unavailable");
        value = storage.secretStorage.read({row.string(0)});
        tx.commit();
    }
    try {
        adapter(value);
    } catch (...) {
        throw Conflict("Secret adapter failed");
    }
    std::lock_guard lock(database.mutex);
    database.exec("INSERT OR IGNORE INTO secret_destinations VALUES(?)", {destination});
}
}
