#pragma once

#include "flock/json.hpp"
#include "flock/policy_action.hpp"
#include "flock/types.hpp"

#include <string_view>

namespace flockv0 {
class Database;
class InboxStorage;
class Policy;
class SecretStorage;
struct Person;

namespace policy_storage {
void audit(Database&, const Person&, std::string_view bot, std::string_view action, const Json& details);
void writeRule(Database&, const SecretStorage&, const Person&, RuleKind, Action, std::string_view target, bool add, bool exact);
void fulfillSecret(Database&, const Policy&, SecretStorage&, InboxStorage&, const Person&, std::string_view bot, Id request,
                   std::string_view value);
}
}
