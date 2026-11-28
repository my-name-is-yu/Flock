#include "model.hpp"
#include "flock/file_descriptor.hpp"
#include "flock/base64.hpp"
#include "flock/json.hpp"
#include "chatgpt_failure.hpp"

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace flockv0::app {
namespace {
using namespace std::chrono_literals;
using chatgpt::field;
using chatgpt::stringField;
constexpr std::size_t maxProfile = 4 * 1024 * 1024;

void checkStop(std::stop_token stop) {
    if (stop.stop_requested()) throw ModelError("ChatGPT sign-in was cancelled", true);
}

bool privateFile(int fd, struct stat& info) {
    return fd >= 0 && fstat(fd, &info) == 0 && info.st_uid == getuid() && (info.st_mode & 0077) == 0;
}

int openDirectory(const std::filesystem::path& directory) {
    std::error_code error;
    if (directory.has_parent_path()) std::filesystem::create_directories(directory.parent_path(), error);
    if (error || (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST))
        throw ModelError("Unable to create Flock's private ChatGPT directory", false);
    return open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
}

int openLock(int directory) {
    struct stat info{};
    if (!privateFile(directory, info) || !S_ISDIR(info.st_mode))
        throw ModelError("Flock's ChatGPT credential directory and lock must be owner-only", false);
    return openat(directory, "lock", O_CREAT | O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600);
}

class ProfileLock {
public:
    explicit ProfileLock(const std::filesystem::path& directory, std::stop_token stop = {}, bool wait = true)
        : dir_(openDirectory(directory)), lock_(openLock(dir_.get())) {
        struct stat info{};
        if (!privateFile(lock_.get(), info) || !S_ISREG(info.st_mode))
            throw ModelError("Flock's ChatGPT credential directory and lock must be owner-only", false);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (::flock(lock_.get(), LOCK_EX | LOCK_NB) != 0) {
            const auto error = errno;
            checkStop(stop);
            if (error != EWOULDBLOCK && error != EINTR) throw ModelError("Unable to lock Flock's ChatGPT credentials", true);
            if (!wait || std::chrono::steady_clock::now() >= deadline)
                throw ModelError("Flock's ChatGPT credentials are busy. Finish browser sign-in or retry shortly.", true);
            std::this_thread::sleep_for(25ms);
        }
        checkStop(stop);
    }

    Json read() const {
        const FileDescriptor file(openat(dir_.get(), "profile.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (file.get() < 0 && errno == ENOENT) return Json::object();
        struct stat info{};
        if (!privateFile(file.get(), info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
            static_cast<std::uintmax_t>(info.st_size) > maxProfile)
            throw ModelError("ChatGPT credential file must be a private regular file", false);
        std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::read(file.get(), bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw ModelError("Unable to read Flock's ChatGPT credentials", false);
            offset += static_cast<std::size_t>(count);
        }
        auto profile = Json::parse(bytes, nullptr, false);
        if (!profile.is_object()) throw ModelError("Unable to read Flock's ChatGPT credentials", false);
        return profile;
    }

    void save(const Json& profile) const {
        auto bytes = profile.dump();
        if (bytes.size() > maxProfile) throw ModelError("Flock's ChatGPT profile exceeded the local limit", false);
        std::array<unsigned char, 18> random{};
        if (RAND_bytes(random.data(), random.size()) != 1) saveFailed();
        auto temporary = ".profile-" + encodeBase64(std::string_view(reinterpret_cast<const char*>(random.data()), random.size()));
        std::ranges::replace(temporary, '/', '_');
        const FileDescriptor file(openat(dir_.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (file.get() < 0) saveFailed();
        try {
            std::string_view remaining(bytes);
            while (!remaining.empty()) {
                const auto count = write(file.get(), remaining.data(), remaining.size());
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) saveFailed();
                remaining.remove_prefix(static_cast<std::size_t>(count));
            }
            if (fsync(file.get()) != 0 || renameat(dir_.get(), temporary.c_str(), dir_.get(), "profile.json") != 0) saveFailed();
        } catch (...) {
            unlinkat(dir_.get(), temporary.c_str(), 0);
            throw;
        }
        if (fsync(dir_.get()) != 0 && errno != EINVAL && errno != ENOTSUP) saveFailed();
    }
private:
    [[noreturn]] static void saveFailed() { throw ModelError("Unable to save Flock's ChatGPT credentials", false); }
    FileDescriptor dir_, lock_;
};

bool hasPlanScope(const Json& profile) {
    const auto& grants = field(profile, "granted");
    return grants.is_array() && std::find(grants.begin(), grants.end(), chatgpt::directScope) != grants.end();
}

bool planUsage(const Json& profile) {
    return field(profile, "signedOut") != true && !stringField(profile, "refreshToken").empty() && hasPlanScope(profile);
}

Json connectionStatus(const Json& profile) {
    const auto connected = !stringField(profile, "subject").empty() && field(profile, "signedOut") != true;
    return {{"connected", connected},
            {"account", connected ? stringField(profile, "email") : ""},
            {"planUsage", planUsage(profile)},
            {"manageUsage", "https://chatgpt.com/settings/usage"}};
}

Json registration(const Json& profile) {
    auto result = Json::object();
    for (const auto* name : {"hostId", "clientId", "subject", "email"}) {
        if (profile.contains(name)) result[name] = profile.at(name);
    }
    return result;
}

double timestamp(const Json& object, const char* key) {
    const auto& value = field(object, key);
    return value.is_number() ? value.get<double>() : 0;
}

bool expiredConnection(std::string_view code) {
    constexpr std::array<std::string_view, 7> codes{"invalid_grant",         "invalid_refresh_token",     "token_expired",
                                                    "refresh_token_expired", "refresh_token_invalidated", "refresh_token_reused",
                                                    "invalid_id_token"};
    return std::ranges::find(codes, code) != codes.end();
}

Json refreshProfile(ProfileLock& lock, Json profile, const chatgpt::HttpTransport& transport) {
    const auto discovery = chatgpt::discover(transport);
    const auto clientId = stringField(profile, "clientId");
    try {
        auto pending = field(profile, "pending");
        if (pending.is_null()) {
            const auto tokens = chatgpt::exchange(
                transport, discovery,
                {{"grant_type", "refresh_token"}, {"client_id", clientId}, {"refresh_token", stringField(profile, "refreshToken")}});
            pending = chatgpt::credentials(tokens, profile);
            if (tokens.contains("id_token")) pending["idToken"] = tokens.at("id_token");
            pending["receivedAt"] = chatgpt::nowMillis();
            // Persist the rotated token before the JWKS request so a retry uses the same response.
            profile["pending"] = pending;
            lock.save(profile);
        }
        if (!pending.is_object()) throw ModelError("Invalid pending ChatGPT credentials", false);
        auto next = registration(profile);
        if (profile.contains("idToken")) next["idToken"] = profile.at("idToken");
        if (pending.contains("idToken")) {
            const auto account =
                chatgpt::identity(transport, pending.at("idToken"), clientId, discovery, std::nullopt, timestamp(pending, "receivedAt"));
            if (field(account, "subject") != field(profile, "subject"))
                throw chatgpt::Failure("Refreshed ChatGPT identity does not match the Flock profile", false, "invalid_id_token");
            next["email"] = field(account, "email");
            next["idToken"] = pending.at("idToken");
        }
        for (const auto* name : {"granted", "accessToken", "refreshToken", "expiresAt", "earliestRefreshAt"}) {
            if (pending.contains(name)) next[name] = pending.at(name);
        }
        lock.save(next);
        return next;
    } catch (const chatgpt::Failure& error) {
        if (!expiredConnection(error.providerCode)) throw;
        lock.save(registration(profile));
        throw ModelError("Flock’s ChatGPT sign-in expired. Sign in to ChatGPT again in Flock settings → Connections.", false);
    }
}
}

ChatGPTModel::ChatGPTModel(std::filesystem::path directory) : ChatGPTModel(std::move(directory), chatgpt::httpRequest) {}

ChatGPTModel::ChatGPTModel(std::filesystem::path directory, chatgpt::HttpTransport transport, chatgpt::BrowserOpener opener)
    : directory_(std::move(directory)), transport_(std::move(transport)), opener_(std::move(opener)) {}

Json ChatGPTModel::login(bool printUrl, bool reconsent) {
    const ProfileLock lock(directory_);
    const auto profile = chatgpt::authorize(transport_, opener_, lock.read(), printUrl, reconsent, false, false, {},
                                            [&](const Json& registration) { lock.save(registration); });
    lock.save(profile);
    return connectionStatus(profile);
}

Json ChatGPTModel::loginWeb(bool reconsent, std::stop_token stop, bool switchAccount, const CommitModelLogin& commit) {
    if (!commit) throw ModelError("Flock needs permission to save a ChatGPT profile", false);
    checkStop(stop);
    const ProfileLock lock(directory_, stop);
    const auto profile = chatgpt::authorize(transport_, opener_, lock.read(), false, reconsent, switchAccount, true, stop, {});
    commit([&] {
        checkStop(stop);
        lock.save(profile);
    });
    return connectionStatus(profile);
}

Json ChatGPTModel::signOut() {
    const ProfileLock lock(directory_);
    const auto profile = lock.read();
    const auto& pending = field(field(profile, "pending"), "refreshToken");
    const auto refreshToken = pending.is_null()     ? stringField(profile, "refreshToken")
                              : pending.is_string() ? pending.get<std::string>()
                                                    : std::string{};
    const auto clientId = stringField(profile, "clientId");
    const auto confirmed = refreshToken.empty() ? field(profile, "revocationConfirmed") != false
                                                : !clientId.empty() && chatgpt::revokeSession(transport_, clientId, refreshToken);
    auto signedOut = registration(profile);
    signedOut["signedOut"] = true;
    signedOut["revocationConfirmed"] = confirmed;
    lock.save(signedOut);
    return {{"revocationConfirmed", confirmed}};
}

Json ChatGPTModel::status() {
    return connectionStatus(ProfileLock(directory_).read());
}

bool ChatGPTModel::hasAuthentication() const {
    try {
        return planUsage(ProfileLock(directory_, {}, false).read());
    } catch (const ModelError&) {
        return false;
    }
}

Json ChatGPTModel::webStatus() {
    const auto profile = ProfileLock(directory_, {}, false).read();
    auto status = connectionStatus(profile);
    status["needsSignIn"] =
        status["connected"] == true &&
        (!field(profile, "granted").is_array() ||
         (hasPlanScope(profile) && (stringField(profile, "accessToken").empty() || stringField(profile, "refreshToken").empty())));
    status["registered"] = !stringField(profile, "clientId").empty();
    return status;
}

std::string ChatGPTModel::accessToken() {
    ProfileLock lock(directory_);
    auto profile = lock.read();
    if (!planUsage(profile))
        throw ModelError("Sign in to ChatGPT in Flock settings → Connections and allow plan usage.",
                         ModelErrorCode::authentication_required);
    const auto now = chatgpt::nowMillis();
    if (!field(profile, "pending").is_null() ||
        (timestamp(profile, "expiresAt") <= now + 60'000 && timestamp(profile, "earliestRefreshAt") <= now))
        profile = refreshProfile(lock, std::move(profile), transport_);
    if (!planUsage(profile))
        throw ModelError(
            "Flock’s ChatGPT plan permission is unavailable. Sign in to ChatGPT again in Flock settings → Connections and allow plan usage.",
            ModelErrorCode::authentication_required);
    if (timestamp(profile, "expiresAt") <= chatgpt::nowMillis())
        throw ModelError("ChatGPT credentials cannot be renewed yet. Retry shortly.", true);
    auto token = stringField(profile, "accessToken");
    if (!chatgpt::validAccessToken(token)) throw ModelError("Flock's saved ChatGPT access token is invalid", false);
    return token;
}

Json ChatGPTModel::models() {
    return chatgpt::models(transport_, accessToken());
}
}
