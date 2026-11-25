#include "flock/identity.hpp"
#include "flock/web.hpp"
#include "flock/store.hpp"

#include <filesystem>
#include <utility>

namespace flockv0 {
void configureWebIdentities(Store& store, const std::filesystem::path& path, WebCallbacks& callbacks) {
    auto identities = loadIdentities(store, path);
    callbacks.authenticate = std::move(identities.authenticate);
    callbacks.resolveIdentity = std::move(identities.resolveRoot);
    callbacks.configuredPerson = std::move(identities.resolvePerson);
}
}
