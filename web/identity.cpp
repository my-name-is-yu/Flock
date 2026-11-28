#include "flock/identity.hpp"
#include "flock/web.hpp"
#include "http.hpp"

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

namespace flockv0::web {
bool hasWebIdentities(const WebCallbacks& callbacks) {
    return callbacks.identityRequired ? callbacks.identityRequired() : bool(callbacks.authenticate);
}

Person person(const WebCallbacks& callbacks, const httplib::Request& request) {
    if (!callbacks.authenticate) return request.has_header("Authorization") ? Person{} : localOwner;
    const auto credentials = request.get_header_value_count("Authorization");
    if (!credentials && callbacks.identityRequired) return callbacks.authenticate({});
    if (credentials != 1) return {};
    return callbacks.authenticate(request.get_header_value("Authorization"));
}
}
