#pragma once

#include "flock/json.hpp"
#include "flock/policy_action.hpp"
#include "flock/types.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace flockv0 {
class SecretRedactor;
class Store;
struct Person;
struct RunRef;

class Policy {
public:
    explicit Policy(Store& store) : store_(store) {}

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
    void executeSessionResource(std::string_view environment, const RunRef&, const Resource&, const std::function<bool()>& upstreamPermits,
                                const std::function<void(Id ownerTask)>& execute) const;
    SecretRedactor secretRedactor(std::string_view bot) const;
    Json redactSecrets(std::string_view bot, const Json& view) const;
    bool requiresIdentity() const;
private:
    Store& store_;
};
}
