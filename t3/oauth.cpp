#include "internal.hpp"
#include "flock/percent_encoding.hpp"
#include "flock/digest.hpp"
#include "flock/json.hpp"
#include "flock/t3_mcp.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

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
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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
    ~Fd() {
        if (fd_ >= 0) close(fd_);
    }
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
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw Failure(Failure::Kind::Wire, "Could not generate T3 OAuth randomness");
    return base64url(bytes.data(), bytes.size());
}

std::string digest(std::string_view value) {
    const auto bytes = sha256(value);
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
        (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode) || st.st_nlink != 1))
        privateStorage();
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
    try {
        verifyPrivate(fd, true);
    } catch (...) {
        if (fd >= 0) close(fd);
        throw;
    }
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

void saveCredential(int directory, const std::string& name, const std::string& environmentId, const Credential& credential) {
    const auto temporary = ".pending-" + randomValue();
    Fd file(openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.get() < 0) privateStorage();
    try {
        const auto data = Json{{"environmentId", environmentId}, {"label", credential.label},           {"mcpUrl", credential.mcpUrl},
                               {"token", credential.token},      {"generation", credential.generation}, {"expiresAt", credential.expiresAt},
                               {"operate", credential.operate}}
                              .dump();
        if (!writeAll(file.get(), data) || fsync(file.get()) != 0 || renameat(directory, temporary.c_str(), directory, name.c_str()) != 0)
            privateStorage();
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
        result += percentEncode(key) + '=' + percentEncode(value);
    }
    return result;
}

Headers parseForm(std::string_view input) {
    Headers result;
    while (!input.empty()) {
        const auto end = input.find('&');
        const auto field = input.substr(0, end);
        const auto equals = field.find('=');
        if (equals == std::string_view::npos || !result.emplace(decode(field.substr(0, equals)), decode(field.substr(equals + 1))).second)
            invalidResponse();
        input.remove_prefix(end == std::string_view::npos ? input.size() : end + 1);
    }
    return result;
}

std::string sameOrigin(const Url& resource, const std::string& value) {
    const auto url = checkedUrl(value);
    if (url.origin != resource.origin) throw Failure(Failure::Kind::Settings, "T3 OAuth discovery advertised a different origin");
    return url.full;
}

Failure oauthFailure(std::string_view stage, const HttpResponse& response) {
    const auto payload = Json::parse(response.body, nullptr, false);
    const auto code = str(payload, "error");
    auto message = "T3 OAuth " + std::string(stage) + " failed (HTTP " + std::to_string(response.status) + ")";
    if (!code.empty()) message += ": " + code;
    return Failure(Failure::Kind::Auth, message);
}

Json oauthJson(std::string_view stage, const std::string& url, const Headers& headers = {}, const std::string& body = {}) {
    const auto response = http(url, body.empty() ? "GET" : "POST", headers, body);
    if (response.status < 200 || response.status >= 300) throw oauthFailure(stage, response);
    auto payload = Json::parse(response.body, nullptr, false);
    if (!payload.is_object() || payload.contains("error")) throw oauthFailure(stage, response);
    return payload;
}

// RFC 9110 challenges and auth-params both use commas. A token followed by
// '=' continues the current challenge. Quoted commas and quoted-pairs are data.
std::optional<std::string> bearerMetadata(std::string_view header) {
    constexpr std::string_view tokens = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'*+-.^_`|~";
    auto space = [&] { header.remove_prefix(std::min(header.find_first_not_of(" \t"), header.size())); };
    auto token = [&] {
        const auto length = std::min(header.find_first_not_of(tokens), header.size());
        const auto value = header.substr(0, length);
        header.remove_prefix(length);
        return value;
    };
    auto lower = [](std::string_view value) {
        std::string result(value);
        for (char& c : result)
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return result;
    };
    bool bearer = false;
    std::optional<std::string> metadata;
    while (!header.empty()) {
        space();
        if (header.empty()) break;
        if (header.front() == ',') {
            header.remove_prefix(1);
            continue;
        }
        auto key = token();
        if (key.empty()) invalidResponse();
        space();
        if (!header.starts_with('=')) {
            bearer = lower(key) == "bearer";
            if (header.empty() || header.starts_with(',')) continue;
            // Other schemes may carry token68 credentials instead of params.
            auto credentials = header.substr(0, header.find(','));
            auto padding = credentials.find('=');
            auto bare = trim(credentials.substr(0, padding));
            if (!bare.empty() &&
                bare.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~+/") == std::string::npos &&
                (padding == std::string_view::npos || trim(credentials.substr(padding)).find_first_not_of('=') == std::string::npos)) {
                header.remove_prefix(credentials.size());
                continue;
            }
            key = token();
            space();
        }
        if (key.empty() || !header.starts_with('=')) invalidResponse();
        header.remove_prefix(1);
        space();
        std::string value;
        if (header.starts_with('"')) {
            header.remove_prefix(1);
            while (!header.empty() && !header.starts_with('"')) {
                if (header.starts_with('\\')) header.remove_prefix(1);
                if (header.empty()) invalidResponse();
                value += header.front();
                header.remove_prefix(1);
            }
            if (header.empty()) invalidResponse();
            header.remove_prefix(1);
        } else {
            value = token();
            if (value.empty()) invalidResponse();
        }
        if (bearer && lower(key) == "resource_metadata") {
            if (metadata && *metadata != value) invalidResponse();
            metadata = std::move(value);
        }
        space();
        if (!header.empty() && !header.starts_with(',')) invalidResponse();
    }
    return metadata;
}

bool supports(const Json& metadata, const char* key, std::string_view value) {
    const auto found = metadata.find(key);
    return found != metadata.end() && found->is_array() && std::find(found->begin(), found->end(), Json(value)) != found->end();
}

struct OAuthEndpoints {
    std::string issuer, authorization, token, registration;
};
OAuthEndpoints discover(const Url& resource) {
    const auto probe =
        http(resource.full, "POST", {{"Accept", "application/json, text/event-stream"}, {"Content-Type", "application/json"}},
             Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", initializeParams()}}.dump());
    if (probe.status != 401) throw oauthFailure("discovery", probe);
    auto metadataUrl = resource.origin + "/.well-known/oauth-protected-resource" + resource.path;
    if (const auto auth = probe.headers.find("www-authenticate"); auth != probe.headers.end()) {
        if (auto advertised = bearerMetadata(auth->second)) metadataUrl = sameOrigin(resource, *advertised);
    }
    const auto metadata = oauthJson("discovery", metadataUrl);
    if (checkedUrl(str(metadata, "resource")).full != resource.full) invalidResponse();
    const auto authorities = metadata.value("authorization_servers", Json::array());
    if (!authorities.is_array() || authorities.size() != 1 || !authorities.front().is_string()) invalidResponse();
    const auto issuer = checkedUrl(authorities.front().get<std::string>());
    if (issuer.origin != resource.origin || issuer.path != "/") invalidResponse();
    const auto oauth = oauthJson("discovery", issuer.origin + "/.well-known/oauth-authorization-server");
    if (checkedUrl(str(oauth, "issuer")).full != issuer.full || !supports(oauth, "code_challenge_methods_supported", "S256") ||
        !supports(oauth, "token_endpoint_auth_methods_supported", "none") ||
        !supports(oauth, "grant_types_supported", "authorization_code") || !supports(oauth, "scopes_supported", OperateScope) ||
        !supports(oauth, "scopes_supported", ReadScope))
        invalidResponse();
    return {str(oauth, "issuer"), sameOrigin(resource, str(oauth, "authorization_endpoint")),
            sameOrigin(resource, str(oauth, "token_endpoint")), sameOrigin(resource, str(oauth, "registration_endpoint"))};
}

int listenOnLoopback() {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) callbackFailure();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fcntl(listener, F_SETFD, FD_CLOEXEC) != 0 || bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener, 4) != 0) {
        close(listener);
        callbackFailure();
    }
    return listener;
}

void checkCancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw Failure(Failure::Kind::Settings, "T3 connection was cancelled");
}

bool ready(int fd, Clock::time_point end, std::stop_token stop) {
    for (;;) {
        checkCancelled(stop);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now());
        if (remaining.count() <= 0) return false;
        pollfd item{fd, POLLIN, 0};
        const auto timeout = static_cast<int>(remaining.count());
        const int result = poll(&item, 1, stop.stop_possible() ? std::min(timeout, 100) : timeout);
        if ((result < 0 && errno == EINTR) || (result == 0 && stop.stop_possible())) continue;
        return result > 0 && (item.revents & POLLIN);
    }
}

std::string requestTarget(int client, Clock::time_point deadline, std::stop_token stop) {
    std::array<char, 2048> buffer{};
    std::string input;
    const auto end = std::min(deadline, Clock::now() + std::chrono::seconds(2));
    const auto lineEnd = [&] { return input.find("\r\n"); };
    while (input.size() < 16384 && lineEnd() == std::string::npos && ready(client, end, stop)) {
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
    const std::string_view text =
        success ? "T3 authorization received. Return to Flock.\n" : "This is not the active Flock authorization callback.\n";
    const auto response =
        std::string(success ? "HTTP/1.1 200 OK" : "HTTP/1.1 400 Bad Request") +
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

std::string receiveCode(int listener, const std::string& state, const std::string& issuer, std::stop_token stop) {
    const auto deadline = Clock::now() + std::chrono::minutes(3);
    while (ready(listener, deadline, stop)) {
        Fd client(accept(listener, nullptr, nullptr));
        if (client.get() < 0 || fcntl(client.get(), F_SETFD, FD_CLOEXEC) != 0) continue;
        const auto target = requestTarget(client.get(), deadline, stop);
        const auto query = target.find('?');
        Headers parameters;
        if (query != std::string::npos && std::string_view(target).substr(0, query) == CallbackPath) {
            try {
                parameters = parseForm(std::string_view(target).substr(query + 1));
            } catch (const Failure&) {
            }
        }
        const auto& receivedState = parameters["state"];
        const bool matches = receivedState.size() == state.size() && CRYPTO_memcmp(receivedState.data(), state.data(), state.size()) == 0 &&
                             parameters["iss"] == issuer;
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

static Credential readCredential(const std::filesystem::path& path, const std::string& environmentId) {
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
    if (!saved.is_object() || str(saved, "environmentId") != environmentId || !saved.contains("expiresAt") ||
        !saved["expiresAt"].is_number_integer() || !saved.contains("operate") || !saved["operate"].is_boolean())
        noCredential();
    const auto url = checkedUrl(str(saved, "mcpUrl"));
    Credential credential{
        url.full,           str(saved, "token"), str(saved, "generation"), saved["expiresAt"].get<Seconds>(), saved["operate"].get<bool>(),
        str(saved, "label")};
    if (credential.label.empty() || url.path != "/mcp" || !validToken(credential.token) || credential.generation.size() != 43 ||
        credential.generation.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
        noCredential();
    return credential;
}

Credential loadCredential(const std::filesystem::path& path, const std::string& environmentId) {
    auto credential = readCredential(path, environmentId);
    if (credential.expiresAt <= unixTime())
        throw Failure(Failure::Kind::Auth, "Sign in to this T3 environment again", Failure::Reason::CredentialExpired);
    return credential;
}

}

namespace {
t3detail::Url checkedResource(const std::string& mcpUrl) {
    auto resource = t3detail::checkedUrl(mcpUrl);
    if (resource.path != "/mcp") throw t3detail::Failure(t3detail::Failure::Kind::Settings, "The T3 URL must end in /mcp");
    return resource;
}
}

std::optional<T3SavedConnection> savedT3Connection(const std::filesystem::path& directory, const std::string& environmentId) {
    try {
        auto credential = t3detail::readCredential(directory, environmentId);
        const bool loopbackHttp = credential.mcpUrl.starts_with("http://");
        return T3SavedConnection{std::move(credential.label), std::move(credential.mcpUrl), credential.expiresAt, loopbackHttp};
    } catch (const t3detail::Failure&) {
        return {};
    }
}

void validateT3McpUrl(const std::string& mcpUrl) {
    try {
        (void)checkedResource(mcpUrl);
    } catch (const t3detail::Failure&) {
        throw Conflict("Use a valid T3 connection URL ending in /mcp");
    }
}

T3LoginResult loginT3(const std::filesystem::path& credentialDirectory, const std::optional<std::string>& expectedEnvironmentId,
                      const std::string& mcpUrl, const std::function<void(const std::string&)>& showAuthorizationUrl, bool requireOperate,
                      const std::function<void(const std::string&, const std::function<void()>&)>& install, std::stop_token stop) {
    using namespace t3detail;
    checkCancelled(stop);
    if (expectedEnvironmentId) (void)credentialName(*expectedEnvironmentId);
    const auto resource = checkedResource(mcpUrl);
    const Fd directory(openDirectory(credentialDirectory, true));
    const auto oauth = discover(resource);
    checkCancelled(stop);
    const Fd listener(listenOnLoopback());
    sockaddr_in addr{};
    socklen_t size = sizeof(addr);
    if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&addr), &size) != 0) callbackFailure();
    const auto redirect = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + std::string(CallbackPath);
    const auto registration = oauthJson("registration", oauth.registration, {{"Content-Type", "application/json"}},
                                        Json{{"client_name", "Flock"},
                                             {"redirect_uris", Json::array({redirect})},
                                             {"token_endpoint_auth_method", "none"},
                                             {"grant_types", Json::array({"authorization_code"})},
                                             {"response_types", Json::array({"code"})}}
                                            .dump());
    checkCancelled(stop);
    const auto clientId = str(registration, "client_id");
    if (clientId.empty() || clientId.size() > 8192) invalidResponse();
    const auto state = randomValue();
    const auto verifier = randomValue();
    const auto scope = std::string(ReadScope) + " " + std::string(OperateScope);
    showAuthorizationUrl(oauth.authorization + '?' +
                         form({{"response_type", "code"},
                               {"client_id", clientId},
                               {"redirect_uri", redirect},
                               {"scope", scope},
                               {"resource", resource.full},
                               {"state", state},
                               {"code_challenge", digest(verifier)},
                               {"code_challenge_method", "S256"}}));
    const auto code = receiveCode(listener.get(), state, oauth.issuer, stop);
    checkCancelled(stop);
    const auto tokenResult = oauthJson("token", oauth.token, {{"Content-Type", "application/x-www-form-urlencoded"}},
                                       form({{"grant_type", "authorization_code"},
                                             {"code", code},
                                             {"redirect_uri", redirect},
                                             {"client_id", clientId},
                                             {"code_verifier", verifier},
                                             {"resource", resource.full}}));
    const auto token = str(tokenResult, "access_token");
    if (!validToken(token) || str(tokenResult, "token_type") != "Bearer" || !tokenResult.contains("expires_in") ||
        !tokenResult["expires_in"].is_number_integer())
        invalidResponse();
    const auto expires = tokenResult["expires_in"].get<Seconds>();
    if (expires <= 0 || expires > 31 * 24 * 60 * 60) invalidResponse();
    bool readScope = false, operateScope = false;
    std::istringstream scopes(str(tokenResult, "scope"));
    for (std::string item; scopes >> item;) {
        readScope |= item == ReadScope;
        operateScope |= item == OperateScope;
    }
    if (!readScope) noCredential();
    if (requireOperate && !operateScope)
        throw Failure(Failure::Kind::Auth, "Authorize T3 read and operate access for the Flock connection");
    checkCancelled(stop);
    Credential credential{resource.full, token, randomValue(), unixTime() + expires, operateScope, {}};
    Client client(credential);
    const auto identity = client.call("t3_environment_read", Json::object());
    auto environmentId = str(identity, "environmentId");
    credential.label = str(identity, "label");
    if (credential.label.empty() || credential.label.size() > 1024) invalidResponse();
    if (expectedEnvironmentId && environmentId != *expectedEnvironmentId)
        throw Failure(Failure::Kind::Settings, "The OAuth endpoint belongs to a different T3 environment");
    const auto name = credentialName(environmentId);
    checkCancelled(stop);
    const auto save = [&] { saveCredential(directory.get(), name, environmentId, credential); };
    if (install)
        install(environmentId, save);
    else
        save();
    return {std::move(environmentId), operateScope};
}
}
