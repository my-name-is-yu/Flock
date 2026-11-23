#pragma once
#include "flock/core.hpp"
#include <compare>

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
    workspace_read,
    workspace_write,
    workspace_exec,
    web_search,
    web_fetch,
    skill_list,
    skill_read,
    attachment_inspect,
    channel_use,
    channel_read,
    channel_post,
    t3_coordinate,
    t3_send,
    automation_change
};
enum class PolicyResult { allow, ask, deny };
std::string_view actionName(Action);
std::optional<Action> parseAction(std::string_view);

struct Person {
    std::string id;
    bool adminRoot = false;
    auto operator<=>(const Person&) const = default;
};
inline const Person localOwner{"local-owner",true};
struct Resource {
    Action action;
    std::string target;
    static Resource sessionRead(std::string_view environment, std::string_view thread);
};
struct SecretRef { std::string name; bool valid() const; };

struct BotChange {
    std::string operation, target, name, requestKey;
};

class Policy {
public:
    PolicyResult check(std::string_view bot, Action, std::string_view target) const;
    PolicyResult check(const Person&, Action, std::string_view target) const;
    PolicyResult check(Id task, Action, std::string_view target) const;
    template<class Origin>
    bool allows(const Origin& origin, Action action, std::string_view target) const {
        return check(origin,action,target) == PolicyResult::allow;
    }
    bool allows(std::string_view bot, const Resource& resource) const {
        return allows(bot,resource.action,resource.target);
    }
    std::string mode() const;
    Json rules() const;
    void changeNeverRule(const Person&, Action, std::string_view target, bool add);
    using PublishOwnedBot = std::function<void(std::string_view, const std::function<void()>&)>;
    Json createOwnedBot(const Person&, const std::function<Json(const PublishOwnedBot&)>&);
    void setOwner(const Person& creator, std::string_view bot);
    void setMember(const Person&, std::string_view bot, std::string_view person, bool add);
    Json administration(const Person&, std::string_view bot) const;
    void executeSessionResource(std::string_view environment, const RunRef&, const Resource&,
                                const std::function<bool()>& upstreamPermits,
                                const std::function<void(Id ownerTask)>& execute) const;
    Json redactSecrets(std::string_view bot, const Json& view) const;
    void requireWebIdentity();
    bool requiresIdentity() const;
    Id applyBotChange(std::string_view bot, const Person& creator, std::string_view operation,
                      std::string_view target, std::string_view name, Id message, int step,
                      const ApplyBotChange&);
    Id requestSecret(std::string_view bot, const SecretRef&, Id sourceMessage = 0);
    void submitSecret(const Person&, std::string_view bot, Id request, std::string_view value);
    void useSecret(Id task, const SecretRef&, bool upstreamPermitted,
                   const std::function<void(std::string_view)>& adapter) const;
private:
    explicit Policy(Store::Impl& db) : db_(db) {}
    void fulfillSecret(const Person&, std::string_view bot, Id request, std::string_view value);
    void audit(const Person&, std::string_view bot, std::string_view action, const Json& details);
    Store::Impl& db_;
    friend class Store;
    friend class RootDelivery;
    friend struct Store::Impl;
};
}
