#pragma once
#include "flock/core.hpp"
#include <compare>

namespace flockv0 {
bool validBotId(std::string_view);
std::string botName(std::string_view);
const Json& resourceOperations();

enum class GrantChange { grant, revoke };
enum class Role { view = 1, decide, admin };
struct Person {
    std::string id;
    bool adminRoot = false;
    auto operator<=>(const Person&) const = default;
};
struct Scope {
    std::string kind, resource, operation;
    static Scope sessionRead(std::string_view environment, std::string_view thread);
    bool valid() const;
};
struct SecretRef { std::string name; bool valid() const; };

struct BotChange {
    std::string operation, target, name, requestKey;
};
struct BotChangePending : Conflict {
    BotChangePending() : Conflict("Approved Bot change still needs to be applied") {}
};
using ApplyBotChange = std::function<void(const BotChange&)>;

class Access {
public:
    bool permits(std::string_view bot, const Scope&, bool upstreamPermitted) const;
    bool permitsTask(Id task, const Scope&, bool upstreamPermitted) const;
    void executeSessionResource(std::string_view environment, const RunRef&, const Scope&,
                                const std::function<bool()>& upstreamPermits,
                                const std::function<void(Id ownerTask)>& execute) const;
    Json redactSecrets(std::string_view bot, const Json& view) const;
    void requireWebIdentity();
    bool requiresIdentity() const;
    bool permitsPersonScope(const Person&, const Scope&) const;
    bool permitsPerson(const Person&, std::string_view bot, Role) const;
    void personGrant(const Person&, std::string_view bot, std::string_view recipient, Role, GrantChange);
    void personScope(const Person& root, std::string_view person, const Scope&, GrantChange);
    void resourceGrant(const Person&, std::string_view bot, const Scope&, GrantChange);
    Id request(std::string_view bot, const Scope&);
    void decide(const Person&, std::string_view bot, Id request, bool approve);
    Json grants(std::string_view bot) const;
    Json administration(const Person&, std::string_view bot) const;
    Id requestBotChange(std::string_view bot, std::string_view operation, std::string_view target, std::string_view name);
    void reviewBotChange(const Person&, std::string_view bot, Id request, bool approve, const ApplyBotChange&);
    Id requestSecret(std::string_view bot, const SecretRef&);
    void submitSecret(const Person&, std::string_view bot, Id request, std::string_view value);
    void useSecret(Id task, const SecretRef&, bool upstreamPermitted,
                   const std::function<void(std::string_view)>& adapter) const;
private:
    explicit Access(Store::Impl& db) : db_(db) {}
    void addGrant(const Person&, std::string_view bot, const Scope&);
    void audit(const Person&, std::string_view bot, std::string_view action, const Json& scope);
    bool holds(const Person&, const Scope&) const;
    Store::Impl& db_;
    friend class Store;
    friend struct Store::Impl;
};
}
