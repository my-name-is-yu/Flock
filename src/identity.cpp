#include "flock/identity.hpp"
#include "database.hpp"
#include "local_machine_storage.hpp"
#include "web_identity_state.hpp"
#include "flock/types.hpp"
#include "flock/person.hpp"
#include "flock/json.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

namespace flockv0 {
void WebIdentity::setWebIdentityResolver(WebIdentityResolver resolver, bool liveInteractiveRoles) {
    std::lock_guard lock(database_.mutex);
    identity_ = {std::move(resolver), liveInteractiveRoles};
}

void WebIdentity::setRootPeople(std::vector<std::string> ids) {
    machines_.setRootPeople(std::move(ids));
}

void WebIdentity::setWebOrigin(std::string origin) {
    machines_.setWebOrigin(std::move(origin));
}

ConfiguredIdentities loadIdentities(Store& store, const std::filesystem::path& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct Close {
        int fd;
        ~Close() {
            if (fd >= 0) close(fd);
        }
    } closeFile{fd};
    struct stat info{};
    if (fd < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 07777) != 0600 ||
        info.st_size <= 0 || info.st_size > 65536)
        throw Conflict("Web identities require an owner-only regular file");
    std::string data(info.st_size, '\0');
    size_t offset = 0;
    while (offset < data.size()) {
        auto count = read(fd, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw Conflict("Cannot read Web identities");
        offset += static_cast<size_t>(count);
    }
    const auto entries = Json::parse(data, nullptr, false);
    if (!entries.is_array()) throw Conflict("Invalid Web identities");
    std::map<std::string, Person, std::less<>> people;
    std::map<std::string, bool, std::less<>> identities;
    std::vector<std::string> roots;
    for (auto& item : entries) {
        try {
            const auto credential = item.at("authorization").get<std::string>();
            Person principal{item.at("person").get<std::string>(), item.value("adminRoot", false)};
            if (principal.id.empty() || credential.size() < 16 ||
                (!credential.starts_with("Basic ") && !credential.starts_with("Bearer ")) ||
                credential.find_first_of("\r\n") != credential.npos || !people.emplace(credential, principal).second)
                throw Conflict("Invalid Web identity");
            const auto identity = identities.try_emplace(principal.id, true).first;
            identity->second = identity->second && principal.adminRoot;
            if (principal.adminRoot) roots.push_back(std::move(principal.id));
        } catch (const Json::exception&) {
            throw Conflict("Invalid Web identity");
        }
    }
    WebIdentity(store).setRootPeople(std::move(roots));
    ConfiguredIdentities configured;
    for (const auto& [person, admin] : identities) configured.users.push_back({person, admin});
    configured.resolveRoot = [identities = std::move(identities)](std::string_view person) -> std::optional<bool> {
        const auto found = identities.find(person);
        if (found == identities.end()) return std::nullopt;
        return found->second;
    };
    configured.resolvePerson = [resolve = configured.resolveRoot](std::string_view id) -> std::optional<Person> {
        const auto root = resolve(id);
        if (!root) return std::nullopt;
        return Person{std::string(id), *root};
    };
    WebIdentity(store).setWebIdentityResolver(configured.resolveRoot);
    configured.authenticate = [people = std::move(people)](std::string_view authorization) -> Person {
        auto found = people.find(authorization);
        return found == people.end() ? Person{} : found->second;
    };
    return configured;
}
}
