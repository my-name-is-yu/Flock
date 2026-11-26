#pragma once
#include "flock/bot_change.hpp"
#include "flock/json.hpp"
#include "flock/person.hpp"
#include "flock/run_ref.hpp"
#include "flock/store.hpp"
#include "flock/types.hpp"

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
bool validBotId(std::string_view);
std::string botName(std::string_view);

enum class Action {
    bot_view,
    bot_talk,
    bot_decide,
    bot_settings,
    bot_create,
    bot_rename,
    rules_manage,
    repository_read,
    repository_write,
    repository_pr,
    repository_review,
    repository_issues,
    secret_use,
    session_read,
    mcp_discover,
    mcp_invoke,
    mcp_read_resource,
    environment_inspect,
    environment_configure,
    environment_launch,
    shared_memory_read,
    shared_memory_write,
    private_memory_write,
    workspace_read,
    workspace_write,
    web_search,
    web_fetch,
    skill_list,
    skill_read,
    attachment_inspect,
    channel_use,
    channel_read,
    channel_post,
    t3_send,
    automation_change,
    email_send,
    destructive_write
};
enum class PolicyResult { allow, ask, deny };
enum class RuleKind { allow, ask, never };
std::optional<RuleKind> parseRuleKind(std::string_view);
std::string_view actionName(Action);
std::optional<Action> parseAction(std::string_view);

struct Resource {
    Action action;
    std::string target;
    // Verified exception to the category default, still subject to rules.
    bool routine = false;
    static Resource sessionRead(std::string_view environment, std::string_view thread);
};
struct SecretRef {
    std::string name;
    bool valid() const;
};
class SecretRedactor;

class Policy {
public:
    PolicyResult check(std::string_view bot, Action, std::string_view target) const;
    PolicyResult check(const Person&, Action, std::string_view target) const;
    PolicyResult check(Id task, Action, std::string_view target) const;
    template <class Origin> bool allows(const Origin& origin, Action action, std::string_view target) const {
        return check(origin, action, target) == PolicyResult::allow;
    }
    bool allows(std::string_view bot, const Resource& resource) const { return allows(bot, resource.action, resource.target); }
    PolicyResult check(std::string_view bot, const Resource&) const;
    bool forbidden(std::string_view bot, Action, std::string_view target) const;
    std::string mode() const;
    void setMode(const Person&, std::string_view);
    void changeRule(const Person&, RuleKind, Action, std::string_view target, bool add);
    Json rules() const;
    void changeNeverRule(const Person&, Action, std::string_view target, bool add);
    using PublishOwnedBot = std::function<void(std::string_view, const std::function<void()>&)>;
    Json createOwnedBot(const Person&, const std::function<Json(const PublishOwnedBot&)>&);
    void setOwner(const Person& creator, std::string_view bot);
    void setMember(const Person&, std::string_view bot, std::string_view person, bool add);
    Json botSettings(const Person&, std::string_view bot) const;
    void executeSessionResource(std::string_view environment, const RunRef&, const Resource&, const std::function<bool()>& upstreamPermits,
                                const std::function<void(Id ownerTask)>& execute) const;
    SecretRedactor secretRedactor(std::string_view bot) const;
    Json redactSecrets(std::string_view bot, const Json& view) const;
    void requireWebIdentity();
    bool requiresIdentity() const;
    struct BotCreationUncertain : std::exception {};
    Id applyBotChange(std::string_view bot, const Person& creator, std::string_view operation, std::string_view target,
                      std::string_view name, Id message, int step, const ApplyBotChange&,
                      const ReconcileBotCreation* onExhausted = nullptr);
    Id requestSecret(std::string_view bot, const SecretRef&, Id sourceMessage = 0);
    void submitSecret(const Person&, std::string_view bot, Id request, std::string_view value);
    void useSecret(Id task, const SecretRef&, std::string_view destination, std::string_view operation, bool upstreamPermitted,
                   const std::function<void(std::string_view)>& adapter) const;
private:
    explicit Policy(Store::Impl& db) : db_(db) {}
    void writeRule(const Person&, RuleKind, Action, std::string_view target, bool add, bool fromCard);
    void fulfillSecret(const Person&, std::string_view bot, Id request, std::string_view value);
    void audit(const Person&, std::string_view bot, std::string_view action, const Json& details);
    Store::Impl& db_;
    friend class AutomationStorage;
    friend class MemoryStorage;
    friend class InboxStorage;
    friend class GitStorage;
    friend class SessionParticipationStorage;
    friend class SessionReadStorage;
    friend class Store;
    friend class ModelCalls;
    friend class TaskOperations;
    friend class TaskRouting;
    friend struct Store::Impl;
    friend class PolicyApprovals;
};
}
