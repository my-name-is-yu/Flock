#pragma once
#include "flock/bot_change.hpp"
#include "flock/json.hpp"
#include "flock/policy_action.hpp"
#include "flock/types.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace flockv0 {
class BotLifecycleStorage;
class Database;
class InboxStorage;
class PolicyApprovals;
class SecretStorage;
class Store;
class TaskRecords;
struct Person;
struct RunRef;

struct SecretRef;
class SecretRedactor;

class Policy {
public:
    explicit Policy(Store&);

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
    Id applyBotChange(std::string_view bot, const Person& creator, std::string_view operation, std::string_view target,
                      std::string_view name, Id message, int step, const ApplyBotChange&,
                      const ReconcileBotCreation* onExhausted = nullptr);
    Id requestSecret(std::string_view bot, const SecretRef&, Id sourceMessage = 0);
    void submitSecret(const Person&, std::string_view bot, Id request, std::string_view value);
    void useSecret(Id task, const SecretRef&, std::string_view destination, std::string_view operation, bool upstreamPermitted,
                   const std::function<void(std::string_view)>& adapter) const;
private:
    Policy(Database&, const BotLifecycleStorage&, const TaskRecords&, SecretStorage&, InboxStorage&, PolicyApprovals&);
    void writeRule(const Person&, RuleKind, Action, std::string_view target, bool add, bool fromCard);
    void fulfillSecret(const Person&, std::string_view bot, Id request, std::string_view value);
    void audit(const Person&, std::string_view bot, std::string_view action, const Json& details);
    Database& database_;
    const BotLifecycleStorage& botLifecycle_;
    const TaskRecords& taskRecords_;
    SecretStorage& secretStorage_;
    InboxStorage& inboxStorage_;
    PolicyApprovals& policyApprovals_;
    friend class InboxStorage;
    friend class PolicyApprovals;
    friend class BotLifecycle;
    friend class Store;
};
}
