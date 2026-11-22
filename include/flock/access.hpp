#pragma once
#include "flock/core.hpp"

namespace flockv0 {
enum class Role { view = 1, decide, admin };
struct Person {
    std::string id;
    bool adminRoot = false; // Assigned by the trusted host, never deserialized from a request.
};
struct Scope {
    std::string kind, resource, operation;
    bool valid() const;
};
struct SecretRef { std::string name; bool valid() const; };

class SecretStore {
public:
    explicit SecretStore(std::filesystem::path directory);
    void insert(const SecretRef&, std::string_view value);
    Json names() const;
    Json redactSecrets(const Json&) const;
private:
    std::filesystem::path directory_;
    std::string read(const SecretRef&) const;
    friend class Access;
};

class Access {
public:
    bool permits(std::string_view bot, const Scope&, bool upstreamPermitted) const;
    bool permitsTask(Id task, const Scope&, bool upstreamPermitted) const;
    void executeSessionResource(std::string_view environment, const RunRef&, const Scope&,
                                const std::function<bool()>& upstreamPermits,
                                const std::function<void()>& execute) const;
    bool requiresIdentity() const;
    bool permitsPerson(const Person&, std::string_view bot, Role) const;
    void personGrant(const Person&, std::string_view bot, std::string_view recipient, Role, bool revoke);
    void personScope(const Person& root, std::string_view person, const Scope&, bool revoke);
    void resourceGrant(const Person&, std::string_view bot, const Scope&, bool revoke);
    Id request(std::string_view bot, const Scope&);
    void decide(const Person&, std::string_view bot, Id request, bool approve);
    Json grants(std::string_view bot) const;
    Json administration(const Person&, std::string_view bot) const;
    void reviewBotChange(const Person&, std::string_view bot, Id request, bool approve);
    Id requestSecret(std::string_view bot, const SecretRef&);
    void submitSecret(const Person&, std::string_view bot, Id request, std::string_view value);
    void useSecret(Id task, const SecretRef&, bool upstreamPermitted,
                   const std::function<void(std::string_view)>& adapter) const;
private:
    explicit Access(Store::Impl& db) : db_(db) {}
    bool holds(const Person&, const Scope&) const;
    Store::Impl& db_;
    friend class Store;
    friend struct Store::Impl;
};
}
