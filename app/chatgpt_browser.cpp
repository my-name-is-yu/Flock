#include "chatgpt.hpp"
#include "flock/model_error.hpp"

#include <openssl/crypto.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

extern char** environ;

namespace flockv0::app::chatgpt {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::string_view callbackPath = "/auth/callback";
constexpr std::string_view completePath = "/auth/complete";

class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) {}
    ~Descriptor() {
        if (fd_ >= 0) close(fd_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

class BrowserProcess {
public:
    explicit BrowserProcess(std::optional<pid_t> pid) : pid_(pid) {}
    ~BrowserProcess() {
        if (result().has_value()) return;
        // The platform opener may outlive the callback. Let it finish without
        // blocking sign-in or leaving an exited child to the daemon.
        try {
            std::thread([pid = *pid_] {
                int status;
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
            }).detach();
        } catch (const std::system_error&) {
        }
    }
    std::optional<bool> result() {
        if (!pid_) return false;
        int status;
        const auto waited = waitpid(*pid_, &status, WNOHANG);
        if (waited == 0 || (waited < 0 && errno == EINTR)) return std::nullopt;
        pid_.reset();
        return waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
private:
    std::optional<pid_t> pid_;
};

void checkStop(std::stop_token stop) {
    if (stop.stop_requested()) throw ModelError("ChatGPT sign-in was cancelled", true);
}

bool readable(int fd, Clock::time_point deadline, std::stop_token stop) {
    for (;;) {
        checkStop(stop);
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining <= 0ms) return false;
        pollfd item{fd, POLLIN, 0};
        const auto timeout = stop.stop_possible() ? std::min(100ms, remaining) : remaining;
        const int ready = poll(&item, 1, static_cast<int>(timeout.count()));
        if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
        return ready > 0 && (item.revents & POLLIN);
    }
}

std::string readHeaders(int client, Clock::time_point deadline, std::stop_token stop) {
    constexpr std::size_t headerLimit = 16 * 1024;
    deadline = std::min(deadline, Clock::now() + 2s);
    std::string request;
    char bytes[2048];
    while (request.size() < headerLimit && readable(client, deadline, stop)) {
        const auto n = recv(client, bytes, std::min(sizeof(bytes), headerLimit - request.size()), MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) break;
        request.append(bytes, static_cast<std::size_t>(n));
        if (request.find("\r\n\r\n") != std::string::npos) return request;
    }
    return {};
}

std::string_view trimHeader(std::string_view value) {
    const auto start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos) return {};
    return value.substr(start, value.find_last_not_of(" \t") - start + 1);
}

std::string_view requestTarget(std::string_view request, std::string_view expectedHost) {
    const auto firstEnd = request.find("\r\n");
    if (firstEnd == std::string_view::npos || !request.starts_with("GET ")) return {};
    const auto first = request.substr(4, firstEnd - 4);
    const auto space = first.find(' ');
    if (space == std::string_view::npos) return {};
    const auto version = first.substr(space + 1);
    const auto target = first.substr(0, space);
    if ((version != "HTTP/1.1" && version != "HTTP/1.0") || !target.starts_with('/') || target.starts_with("//") ||
        std::ranges::any_of(target, [](unsigned char c) { return c <= ' ' || c == 127 || c == '#'; }))
        return {};

    request.remove_prefix(firstEnd + 2);
    bool sawHost = false;
    while (!request.starts_with("\r\n")) {
        const auto end = request.find("\r\n");
        if (end == std::string_view::npos) return {};
        const auto header = request.substr(0, end);
        const auto colon = header.find(':');
        if (colon == std::string_view::npos || header.starts_with(' ') || header.starts_with('\t')) return {};
        if (colon == 4 && strncasecmp(header.data(), "Host", 4) == 0) {
            if (sawHost || trimHeader(header.substr(colon + 1)) != expectedHost) return {};
            sawHost = true;
        }
        request.remove_prefix(end + 2);
    }
    return sawHost ? target : std::string_view{};
}

std::string decodeFormField(std::string_view field) {
    std::string result;
    for (std::size_t index = 0; index < field.size(); ++index) {
        const auto c = field[index];
        if (c == '%' && index + 2 < field.size()) {
            unsigned int byte = 0;
            const auto start = field.data() + index + 1;
            const auto parsed = std::from_chars(start, start + 2, byte, 16);
            if (parsed.ec == std::errc{} && parsed.ptr == start + 2) {
                result.push_back(static_cast<char>(byte));
                index += 2;
                continue;
            }
        }
        result.push_back(c == '+' ? ' ' : c);
    }
    return result;
}

Form queryFields(std::string_view query) {
    Form fields;
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto part = query.substr(0, amp);
        const auto equal = part.find('=');
        fields.emplace_back(decodeFormField(part.substr(0, equal)),
                            equal == std::string_view::npos ? "" : decodeFormField(part.substr(equal + 1)));
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return fields;
}

std::size_t fieldCount(const Form& fields, std::string_view name) {
    return std::ranges::count(fields, name, &Form::value_type::first);
}

std::string_view formField(const Form& fields, std::string_view name, std::string_view fallback = {}) {
    const auto it = std::ranges::find(fields, name, &Form::value_type::first);
    return it == fields.end() ? fallback : std::string_view(it->second);
}

void reply(int client, std::string_view status, std::string_view body = {}, std::string_view headers = {}) {
    auto response = "HTTP/1.1 " + std::string(status) +
                    "\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n"
                    "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\nContent-Type: text/plain; charset=utf-8\r\n"
                    "Connection: close\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\n" + std::string(headers) + "\r\n" + std::string(body);
#ifdef SO_NOSIGPIPE
    const int on = 1;
    if (setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) != 0) return;
    const int flags = MSG_DONTWAIT;
#else
    const int flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#endif
    std::string_view rest = response;
    while (!rest.empty()) {
        const auto sent = send(client, rest.data(), rest.size(), flags);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return;
        rest.remove_prefix(static_cast<std::size_t>(sent));
    }
}

[[noreturn]] void listenerFailure() {
    throw ModelError("Could not receive the ChatGPT OAuth callback on loopback", true);
}

int acceptClient(int listener) {
#ifdef __linux__
    const auto client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
#else
    const auto client = accept(listener, nullptr, nullptr);
    if (client >= 0 && fcntl(client, F_SETFD, FD_CLOEXEC) != 0) {
        close(client);
        listenerFailure();
    }
#endif
    if (client < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED) listenerFailure();
    return client;
}

bool validClient(std::string_view client) {
    return !client.empty() && client.size() <= 200 && client != "dynamic_agent_client" &&
           client.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string_view::npos;
}
}

std::optional<pid_t> openBrowser(const std::string& url) {
#ifdef __APPLE__
    char command[] = "/usr/bin/open";
#else
    char command[] = "xdg-open";
#endif
    auto argument = url;
    char* args[] = {command, argument.data(), nullptr};
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return std::nullopt;
    bool ready = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
                 posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
                 posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
    posix_spawnattr_t* attributes = nullptr;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_t closeDescriptors;
    if (posix_spawnattr_init(&closeDescriptors) == 0) {
        attributes = &closeDescriptors;
        ready = ready && posix_spawnattr_setflags(attributes, POSIX_SPAWN_CLOEXEC_DEFAULT) == 0;
    } else {
        ready = false;
    }
#endif
    pid_t pid;
    const bool started = ready && posix_spawnp(&pid, command, &actions, attributes, args, environ) == 0;
    if (attributes) posix_spawnattr_destroy(attributes);
    posix_spawn_file_actions_destroy(&actions);
    return started ? std::optional{pid} : std::nullopt;
}

Authorization authorizeBrowser(const BrowserOpener& open, const std::string& endpoint, Form parameters, const std::string& savedClient,
                               bool printUrl, bool browserRequired, std::stop_token stop) {
    checkStop(stop);
#ifdef __linux__
    const Descriptor listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
#else
    const Descriptor listener(socket(AF_INET, SOCK_STREAM, 0));
    if (listener.get() < 0 || fcntl(listener.get(), F_SETFD, FD_CLOEXEC) != 0 || fcntl(listener.get(), F_SETFL, O_NONBLOCK) != 0)
        listenerFailure();
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (listener.get() < 0 || bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), size) != 0 ||
        listen(listener.get(), 8) != 0 || getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &size) != 0)
        listenerFailure();
    const auto host = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    const auto redirectUri = "http://" + host + std::string(callbackPath);
    parameters.emplace_back("redirect_uri", redirectUri);
    const auto state = formField(parameters, "state");
    const auto url = endpoint.substr(0, endpoint.find_first_of("?#")) + '?' + formEncode(parameters);
    const auto deadline = Clock::now() + 5min;

    if (!browserRequired) std::cerr << "Continue with ChatGPT. Approve Flock's plan access in your browser.\n";
    const bool manual = printUrl && !browserRequired;
    if (manual) std::cerr << url << '\n';
    BrowserProcess opener(manual ? std::nullopt : open(url));
    bool openerPending = !manual;

    std::optional<Form> callback;
    auto completionDeadline = deadline;
    while (Clock::now() < completionDeadline) {
        const auto next = openerPending && !callback ? std::min(deadline, Clock::now() + 25ms) : completionDeadline;
        if (!readable(listener.get(), next, stop)) {
            if (openerPending && !callback) {
                if (const auto result = opener.result()) {
                    openerPending = false;
                    if (!*result) {
                        if (browserRequired) throw ModelError("Flock could not open a browser for ChatGPT sign-in on this machine", false);
                        std::cerr << url << '\n';
                    }
                }
            }
            continue;
        }
        const Descriptor client(acceptClient(listener.get()));
        if (client.get() < 0) continue;
        const auto request = readHeaders(client.get(), completionDeadline, stop);
        const auto target = requestTarget(request, host);
        const auto query = target.find('?');
        const auto path = target.substr(0, query);
        if (path == completePath) {
            reply(client.get(), "200 OK", "Return to Flock to check your ChatGPT connection.");
            if (callback) break;
        } else if (path == callbackPath && !callback) {
            auto fields = queryFields(query == std::string_view::npos ? std::string_view{} : target.substr(query + 1));
            const auto receivedState = formField(fields, "state");
            if (fieldCount(fields, "state") != 1 || receivedState.size() != state.size() ||
                CRYPTO_memcmp(receivedState.data(), state.data(), state.size()) != 0) {
                reply(client.get(), "400 Bad Request", "Invalid sign-in state.");
                continue;
            }
            reply(client.get(), "303 See Other", {}, "Location: /auth/complete\r\n");
            callback.emplace(std::move(fields));
            // Let the browser remove the authorization data from its address bar.
            completionDeadline = std::min(deadline, Clock::now() + 500ms);
        } else {
            reply(client.get(), target.empty() ? "400 Bad Request" : "404 Not Found");
        }
    }
    checkStop(stop);
    if (!callback) throw ModelError("ChatGPT browser approval timed out", false);
    if (fieldCount(*callback, "error")) throw ModelError("ChatGPT authorization was declined or expired", false);
    const auto code = formField(*callback, "code");
    const auto clientId = formField(*callback, "client_id", savedClient);
    if (fieldCount(*callback, "code") != 1 || code.empty() || fieldCount(*callback, "client_id") > 1 || !validClient(clientId) ||
        (!savedClient.empty() && clientId != savedClient))
        throw ModelError("ChatGPT did not return a valid Flock client registration", false);
    return {std::string(code), std::string(clientId), redirectUri};
}
}
