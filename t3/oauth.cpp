#include "internal.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <sstream>

namespace flockv0 {
namespace t3detail {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::string_view ReadScope = "orchestration:read";
constexpr std::string_view OperateScope = "orchestration:operate";
constexpr std::string_view CallbackPath = "/flock/oauth/callback";

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

[[noreturn]] void privateStorage() {
    throw Failure(Failure::Kind::Settings, "T3 credentials require a private, owner-only directory and files");
}
[[noreturn]] void noCredential() {
    throw Failure(Failure::Kind::Auth, "Sign in to this T3 environment again");
}
[[noreturn]] void callbackFailure() {
    throw Failure(Failure::Kind::Auth, "Could not receive the T3 OAuth callback on loopback");
}

std::string base64url(const unsigned char* data, int size) {
    std::string result(4 * ((size + 2) / 3) + 1, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), data, size);
    if (written < 0) invalidResponse();
    result.resize(written);
    while (result.ends_with('=')) result.pop_back();
    for (auto& c : result) {
        if (c == '+') c = '-';
        if (c == '/') c = '_';
    }
    return result;
}

std::string randomValue() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1)
        throw Failure(Failure::Kind::Wire, "Could not generate T3 OAuth randomness");
    return base64url(bytes.data(), bytes.size());
}

std::string digest(std::string_view value) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> bytes{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes.data());
    return base64url(bytes.data(), bytes.size());
}

std::string credentialName(const std::string& environmentId) {
    if (trim(environmentId) != environmentId || environmentId.empty() || environmentId.size() > 512)
        throw Failure(Failure::Kind::Settings, "A canonical T3 environment ID is required");
    return "t3-" + digest(environmentId) + ".json";
}

void verifyPrivate(int fd, bool directory) {
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 0077) ||
        (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode) || st.st_nlink != 1)) privateStorage();
}

int openDirectory(const std::filesystem::path& path, bool create) {
    if (path.empty() || path.filename().empty() || path.filename() == "." || path.filename() == "..") privateStorage();
    if (create) {
        std::error_code error;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
        if (error || (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)) privateStorage();
    }
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && !create) noCredential();
    try { verifyPrivate(fd, true); }
    catch (...) { if (fd >= 0) close(fd); throw; }
    return fd;
}

bool writeAll(int fd, std::string_view text) {
    while (!text.empty()) {
        const auto n = write(fd, text.data(), text.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        text.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

void saveCredential(int directory, const std::string& name, const std::string& environmentId,
                    const Credential& credential) {
    const auto temporary = ".pending-" + randomValue();
    Fd file(openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.get() < 0) privateStorage();
    try {
        const auto data = Json{{"environmentId", environmentId}, {"mcpUrl", credential.mcpUrl},
                               {"token", credential.token}, {"generation", credential.generation},
                               {"expiresAt", credential.expiresAt}, {"operate", credential.operate}}.dump();
        if (!writeAll(file.get(), data) || fsync(file.get()) != 0 ||
            renameat(directory, temporary.c_str(), directory, name.c_str()) != 0) privateStorage();
        if (fsync(directory) != 0 && errno != EINVAL && errno != ENOTSUP) privateStorage();
    } catch (...) {
        unlinkat(directory, temporary.c_str(), 0);
        throw;
    }
}

std::string form(const Headers& parameters) {
    std::string result;
    for (const auto& [key, value] : parameters) {
        if (!result.empty()) result += '&';
        result += encode(key) + '=' + encode(value);
    }
    return result;
}

Headers parseForm(std::string_view input) {
    Headers result;
    while (!input.empty()) {
        const auto end = input.find('&');
        const auto field = input.substr(0, end);
        const auto equals = field.find('=');
        if (equals == std::string_view::npos ||
            !result.emplace(decode(field.substr(0, equals)), decode(field.substr(equals + 1))).second)
            invalidResponse();
        input.remove_prefix(end == std::string_view::npos ? input.size() : end + 1);
    }
    return result;
}

std::string sameOrigin(const Url& resource, const std::string& value) {
    const auto url = checkedUrl(value);
    if (url.origin != resource.origin)
        throw Failure(Failure::Kind::Settings, "T3 OAuth discovery advertised a different origin");
    return url.full;
}

Json oauthJson(const std::string& url, const Headers& headers = {}, const std::string& body = {}) {
    const auto response = http(url, body.empty() ? "GET" : "POST", headers, body);
    if (response.status < 200 || response.status >= 300)
        throw Failure(Failure::Kind::Auth, "T3 rejected OAuth discovery, registration, or token exchange");
    auto payload = parse(response.body);
    if (!payload.is_object()) invalidResponse();
    return payload;
}

bool supports(const Json& metadata, const char* key, std::string_view value) {
    const auto found = metadata.find(key);
    return found != metadata.end() && found->is_array() &&
        std::find(found->begin(), found->end(), Json(value)) != found->end();
}

struct OAuthEndpoints { std::string issuer, authorization, token, registration; };
OAuthEndpoints discover(const Url& resource) {
    const auto probe = http(resource.full, "POST", {{"Accept", "application/json, text/event-stream"},
                            {"Content-Type", "application/json"}},
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"Flock","version":"0.1.0"}}})");
    if (probe.status != 401)
        throw Failure(Failure::Kind::Settings, "T3 did not advertise OAuth at this MCP URL");
    auto metadataUrl = resource.origin + "/.well-known/oauth-protected-resource" + resource.path;
    const auto auth = probe.headers.find("www-authenticate");
    constexpr std::string_view marker = "resource_metadata=\"";
    if (auth != probe.headers.end()) {
        const auto offset = auth->second.find(marker);
        if (offset != std::string::npos) {
            const auto begin = offset + marker.size();
            const auto end = auth->second.find('"', begin);
            if (end == std::string::npos) invalidResponse();
            metadataUrl = sameOrigin(resource, auth->second.substr(begin, end - begin));
        }
    }
    const auto metadata = oauthJson(metadataUrl);
    if (checkedUrl(str(metadata, "resource")).full != resource.full) invalidResponse();
    const auto authorities = metadata.value("authorization_servers", Json::array());
    if (!authorities.is_array() || authorities.size() != 1 || !authorities.front().is_string()) invalidResponse();
    const auto issuer = checkedUrl(authorities.front().get<std::string>());
    if (issuer.origin != resource.origin || issuer.path != "/") invalidResponse();
    const auto oauth = oauthJson(issuer.origin + "/.well-known/oauth-authorization-server");
    if (checkedUrl(str(oauth, "issuer")).full != issuer.full ||
        !supports(oauth, "code_challenge_methods_supported", "S256") ||
        !supports(oauth, "token_endpoint_auth_methods_supported", "none") ||
        !supports(oauth, "grant_types_supported", "authorization_code") ||
        !supports(oauth, "scopes_supported", OperateScope) ||
        !supports(oauth, "scopes_supported", ReadScope)) invalidResponse();
    return {str(oauth, "issuer"), sameOrigin(resource, str(oauth, "authorization_endpoint")),
            sameOrigin(resource, str(oauth, "token_endpoint")), sameOrigin(resource, str(oauth, "registration_endpoint"))};
}

int listenOnLoopback() {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) callbackFailure();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fcntl(listener, F_SETFD, FD_CLOEXEC) != 0 ||
        bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 4) != 0) {
        close(listener);
        callbackFailure();
    }
    return listener;
}

bool ready(int fd, Clock::time_point end) {
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now());
        if (remaining.count() <= 0) return false;
        pollfd item{fd, POLLIN, 0};
        const int result = poll(&item, 1, static_cast<int>(remaining.count()));
        if (result < 0 && errno == EINTR) continue;
        return result > 0 && (item.revents & POLLIN);
    }
}

std::string requestTarget(int client, Clock::time_point deadline) {
    std::array<char, 2048> buffer{};
    std::string input;
    const auto end = std::min(deadline, Clock::now() + std::chrono::seconds(2));
    const auto lineEnd = [&] { return input.find("\r\n"); };
    while (input.size() < 16384 && lineEnd() == std::string::npos && ready(client, end)) {
        const auto received = recv(client, buffer.data(), buffer.size(), 0);
        if (received <= 0) return {};
        input.append(buffer.data(), static_cast<std::size_t>(received));
    }
    if (lineEnd() == std::string::npos || !input.starts_with("GET ")) return {};
    const auto targetEnd = input.find(' ', 4);
    if (targetEnd == std::string::npos || targetEnd >= lineEnd()) return {};
    return input.substr(4, targetEnd - 4);
}

void callbackReply(int client, bool success) {
    const std::string_view text = success ? "T3 authorization received. Return to Flock.\n" :
                                            "This is not the active Flock authorization callback.\n";
    const auto response = std::string(success ? "HTTP/1.1 200 OK" : "HTTP/1.1 400 Bad Request") +
        "\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
        std::to_string(text.size()) + "\r\n\r\n" + std::string(text);
#ifdef SO_NOSIGPIPE
    const int on = 1;
    if (setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) != 0) return;
    send(client, response.data(), response.size(), MSG_DONTWAIT);
#else
    send(client, response.data(), response.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
#endif
}

std::string receiveCode(int listener, const std::string& state, const std::string& issuer) {
    const auto deadline = Clock::now() + std::chrono::minutes(3);
    while (ready(listener, deadline)) {
        Fd client(accept(listener, nullptr, nullptr));
        if (client.get() < 0 || fcntl(client.get(), F_SETFD, FD_CLOEXEC) != 0) continue;
        const auto target = requestTarget(client.get(), deadline);
        const auto query = target.find('?');
        Headers parameters;
        if (query != std::string::npos && std::string_view(target).substr(0, query) == CallbackPath) {
            try { parameters = parseForm(std::string_view(target).substr(query + 1)); }
            catch (const Failure&) {}
        }
        const auto& receivedState = parameters["state"];
        const bool matches = receivedState.size() == state.size() &&
            CRYPTO_memcmp(receivedState.data(), state.data(), state.size()) == 0 && parameters["iss"] == issuer;
        callbackReply(client.get(), matches);
        if (!matches) continue;
        if (parameters.contains("error") || parameters["code"].empty())
            throw Failure(Failure::Kind::Auth, "T3 OAuth authorization was denied or cancelled");
        return parameters["code"];
    }
    throw Failure(Failure::Kind::Auth, "Timed out waiting for browser approval of T3 OAuth");
}

bool validToken(std::string_view token) {
    return !token.empty() && token.size() <= 8192 &&
           token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~+/=") == std::string_view::npos;
}
}

Credential loadCredential(const std::filesystem::path& path, const std::string& environmentId) {
    const auto name = credentialName(environmentId);
    const Fd directory(openDirectory(path, false));
    const Fd file(openat(directory.get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (file.get() < 0 && errno == ENOENT) noCredential();
    verifyPrivate(file.get(), false);
    std::array<char, 4096> buffer{};
    std::string bytes;
    for (;;) {
        const auto n = ::read(file.get(), buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || bytes.size() + static_cast<std::size_t>(n) > 32768) privateStorage();
        if (n == 0) break;
        bytes.append(buffer.data(), static_cast<std::size_t>(n));
    }
    const auto saved = Json::parse(bytes, nullptr, false);
    if (!saved.is_object() || str(saved, "environmentId") != environmentId ||
        !saved.contains("expiresAt") || !saved["expiresAt"].is_number_integer() ||
        !saved.contains("operate") || !saved["operate"].is_boolean()) noCredential();
    const auto url = checkedUrl(str(saved, "mcpUrl"));
    Credential credential{url.full, str(saved, "token"), str(saved, "generation"),
                          saved["expiresAt"].get<Seconds>(), saved["operate"].get<bool>()};
    if (url.path != "/mcp" || !validToken(credential.token) || credential.generation.size() != 43 ||
        credential.generation.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos ||
        credential.expiresAt <= unixTime()) noCredential();
    return credential;
}

Credential readCredential(const std::filesystem::path& directory, const std::string& environment, const Json& config) {
    auto connection = loadCredential(directory, environment);
    if (!config.is_object() || (config.contains("t3") && !config["t3"].is_object()))
        throw Failure(Failure::Kind::Settings, "Invalid T3 Bot settings");
    if (config.contains("t3") && config["t3"].contains("mcpUrl")) {
        const auto hint = str(config["t3"], "mcpUrl");
        if (checkedUrl(hint).full != connection.mcpUrl)
            throw Failure(Failure::Kind::Settings, "The Bot's MCP URL differs from the T3 environment's OAuth connection");
    }
    return connection;
}
}

void loginT3(const std::filesystem::path& credentialDirectory, const std::string& environmentId,
             const std::string& mcpUrl, const std::function<void(const std::string&)>& showAuthorizationUrl) {
    using namespace t3detail;
    const auto name = credentialName(environmentId);
    const auto resource = checkedUrl(mcpUrl);
    if (resource.path != "/mcp") throw Failure(Failure::Kind::Settings, "The T3 URL must end in /mcp");
    const Fd directory(openDirectory(credentialDirectory, true));
    const auto oauth = discover(resource);
    const Fd listener(listenOnLoopback());
    sockaddr_in addr{};
    socklen_t size = sizeof(addr);
    if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&addr), &size) != 0) callbackFailure();
    const auto redirect = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + std::string(CallbackPath);
    const auto registration = oauthJson(oauth.registration, {{"Content-Type", "application/json"}},
        Json{{"client_name", "Flock"}, {"redirect_uris", Json::array({redirect})},
             {"token_endpoint_auth_method", "none"}, {"grant_types", Json::array({"authorization_code"})},
             {"response_types", Json::array({"code"})}}.dump());
    const auto clientId = str(registration, "client_id");
    if (clientId.empty() || clientId.size() > 8192) invalidResponse();
    const auto state = randomValue();
    const auto verifier = randomValue();
    const auto scope = std::string(ReadScope) + " " + std::string(OperateScope);
    showAuthorizationUrl(oauth.authorization + '?' + form({{"response_type", "code"}, {"client_id", clientId},
        {"redirect_uri", redirect}, {"scope", scope}, {"resource", resource.full}, {"state", state},
        {"code_challenge", digest(verifier)}, {"code_challenge_method", "S256"}}));
    const auto code = receiveCode(listener.get(), state, oauth.issuer);
    const auto tokenResult = oauthJson(oauth.token, {{"Content-Type", "application/x-www-form-urlencoded"}},
        form({{"grant_type", "authorization_code"}, {"code", code}, {"redirect_uri", redirect},
              {"client_id", clientId}, {"code_verifier", verifier}, {"resource", resource.full}}));
    const auto token = str(tokenResult, "access_token");
    if (!validToken(token) || str(tokenResult, "token_type") != "Bearer" ||
        !tokenResult.contains("expires_in") || !tokenResult["expires_in"].is_number_integer()) invalidResponse();
    const auto expires = tokenResult["expires_in"].get<Seconds>();
    if (expires <= 0 || expires > 31 * 24 * 60 * 60) invalidResponse();
    bool readScope = false, operateScope = false;
    std::istringstream scopes(str(tokenResult, "scope"));
    for (std::string item; scopes >> item;) {
        readScope |= item == ReadScope;
        operateScope |= item == OperateScope;
    }
    if (!readScope) noCredential();
    Credential credential{resource.full, token, randomValue(), unixTime() + expires, operateScope};
    Client client(credential);
    const auto identity = client.call("t3_environment_read", Json::object());
    if (str(identity, "environmentId") != environmentId)
        throw Failure(Failure::Kind::Settings, "The OAuth endpoint belongs to a different T3 environment");
    saveCredential(directory.get(), name, environmentId, credential);
}
}
