#pragma once
#include "flock/access.hpp"

namespace flockv0 {

struct ConfiguredIdentities {
    std::function<Person(std::string_view authorization)> authenticate;
    WebIdentityResolver resolveRoot;
    std::function<std::optional<Person>(std::string_view personId)> resolvePerson;
};

// Installs the resolved identities for access checks on persisted requests.
ConfiguredIdentities loadIdentities(Store&, const std::filesystem::path&);

}
