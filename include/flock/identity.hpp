#pragma once
#include "flock/person.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Store;

struct ConfiguredIdentities {
    std::function<Person(std::string_view authorization)> authenticate;
    WebIdentityResolver resolveRoot;
    std::function<std::optional<Person>(std::string_view personId)> resolvePerson;
    std::vector<Person> users = {};
};

// Installs the resolved identities for access checks on persisted requests.
ConfiguredIdentities loadIdentities(Store&, const std::filesystem::path&);

}
