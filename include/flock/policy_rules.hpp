#pragma once

#include "flock/json.hpp"
#include "flock/policy_action.hpp"

#include <string_view>

namespace flockv0 {
class Store;
struct Person;

class PolicyRules {
public:
    explicit PolicyRules(Store& store) : store_(store) {}

    void setMode(const Person&, std::string_view);
    void changeRule(const Person&, RuleKind, Action, std::string_view target, bool add);
    Json rules() const;
    void changeNeverRule(const Person&, Action, std::string_view target, bool add);
    void requireWebIdentity();
private:
    Store& store_;
};
}
