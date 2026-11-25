#include "../chatgpt.hpp"
#include "fixtures.hpp"

#include <arpa/inet.h>
#include <spawn.h>
#include <sys/socket.h>

#include <cerrno>
#include <charconv>
#include <future>
#include <iostream>
#include <sstream>

extern char** environ;

namespace {
using namespace flockv0;
using namespace flockv0::app::chatgpt;
using flockv0::app::test::check;
using namespace std::chrono_literals;

const std::string authorizationEndpoint = "https://auth.openai.com/authorize";

int callbackPort(std::string_view url) {
    constexpr std::string_view parameter = "redirect_uri=http%3A%2F%2F127.0.0.1%3A";
    const auto start = url.find(parameter);
    check(url.starts_with(authorizationEndpoint + '?') && start != url.npos, "Missing loopback redirect URI");
    url.remove_prefix(start + parameter.size());
    int port = 0;
    const auto parsed = std::from_chars(url.data(), url.data() + url.size(), port);
    check(parsed.ec == std::errc{} && port > 0 && port <= 65535 && url.substr(parsed.ptr - url.data()).starts_with("%2Fauth%2Fcallback"),
          "Invalid loopback redirect URI");
    return port;
}

class LoopbackRequest {
    struct Socket {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        ~Socket() {
            if (fd >= 0) close(fd);
        }
    } socket_;
public:
    LoopbackRequest(int port, std::string_view target, bool validHost = true) {
        check(socket_.fd >= 0, "Cannot create loopback test socket");
        const timeval timeout{3, 0};
        check(setsockopt(socket_.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "Cannot set loopback timeout");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        check(connect(socket_.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "Cannot connect to callback");
        const auto request = "GET " + std::string(target) + " HTTP/1.1\r\nHost: " + (validHost ? "127.0.0.1:" : "localhost:") +
                             std::to_string(port) + "\r\n\r\n";
        check(send(socket_.fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()), "Cannot send callback");
    }
    LoopbackRequest(const LoopbackRequest&) = delete;
    LoopbackRequest& operator=(const LoopbackRequest&) = delete;

    std::string response() {
        std::string result;
        char bytes[1024];
        for (;;) {
            const auto n = recv(socket_.fd, bytes, sizeof(bytes), 0);
            if (n < 0 && errno == EINTR) continue;
            check(n >= 0, "Cannot read callback response");
            if (n == 0) return result;
            result.append(bytes, static_cast<std::size_t>(n));
        }
    }
};

std::optional<pid_t> simulatedBrowser() {
    char shell[] = "/bin/sh", command[] = "-c", script[] = "exit 0";
    char* arguments[] = {shell, command, script, nullptr};
    pid_t pid = -1;
    check(posix_spawn(&pid, shell, nullptr, nullptr, arguments, environ) == 0, "Cannot start simulated browser");
    return pid;
}

class CaptureStderr final : public std::stringbuf {
public:
    explicit CaptureStderr(std::stop_source* stop = nullptr) : stop_(stop), previous_(std::cerr.rdbuf(this)) {}
    ~CaptureStderr() override { std::cerr.rdbuf(previous_); }
private:
    int sync() override {
        if (stop_ && view().ends_with('\n') && view().find(authorizationEndpoint) != std::string_view::npos) stop_->request_stop();
        return std::stringbuf::sync();
    }
    std::stop_source* stop_;
    std::streambuf* previous_;
};

template <class Action> void expectModelError(Action action, std::string_view message, bool transient) {
    try {
        action();
    } catch (const ModelError& error) {
        check(error.transient == transient && std::string_view(error.what()).find(message) != std::string_view::npos,
              "Unexpected login error: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("Expected login error: " + std::string(message));
}

void callbackChecks() {
    std::stop_source stop;
    std::promise<std::string> opened;
    auto url = opened.get_future();
    auto login = std::async(std::launch::async, [&] {
        return authorizeBrowser(
            [&](const std::string& value) {
                const auto pid = simulatedBrowser();
                opened.set_value(value);
                return pid;
            },
            authorizationEndpoint, {{"state", "fixture-state"}}, "", false, true, stop.get_token());
    });
    try {
        check(url.wait_for(3s) == std::future_status::ready, "Sign-in did not open the browser");
        const auto port = callbackPort(url.get());
        const auto callback = "/auth/callback?state=fixture-state&client_id=fixture-client&code=fixture%2Bcode";
        check(LoopbackRequest(port, callback, false).response().starts_with("HTTP/1.1 400"), "Invalid Host was accepted");
        check(LoopbackRequest(port, "/auth/callback?state=wrong&code=fixture").response().starts_with("HTTP/1.1 400"),
              "Wrong state was accepted");
        check(LoopbackRequest(port, std::string(callback) + "&state=fixture-state").response().starts_with("HTTP/1.1 400"),
              "Duplicate state was accepted");

        const auto redirect = LoopbackRequest(port, callback).response();
        check(redirect.starts_with("HTTP/1.1 303") && redirect.find("\r\nLocation: /auth/complete\r\n") != redirect.npos &&
                  redirect.find("\r\nReferrer-Policy: no-referrer\r\n") != redirect.npos &&
                  redirect.find("\r\nCache-Control: no-store\r\n") != redirect.npos && redirect.find("fixture") == redirect.npos,
              "Callback did not clear authorization data from the browser");
        const auto completion = LoopbackRequest(port, "/auth/complete").response();
        check(completion.starts_with("HTTP/1.1 200") && completion.find("Return to Flock") != completion.npos,
              "The browser could not load the sign-in completion page");
        const auto result = login.get();
        check(result.code == "fixture+code" && result.clientId == "fixture-client", "Callback returned the wrong authorization data");
    } catch (...) {
        stop.request_stop();
        throw;
    }
}

void openerFailure() {
    const BrowserOpener failed = [](const std::string&) { return std::nullopt; };
    {
        CaptureStderr capture;
        expectModelError([&] { authorizeBrowser(failed, authorizationEndpoint, {{"state", "fixture-state"}}, "", false, true, {}); },
                         "could not open a browser", false);
        check(capture.view().find(authorizationEndpoint) == std::string_view::npos, "Web sign-in printed the authorization URL");
    }
    {
        std::stop_source stop;
        CaptureStderr capture(&stop);
        expectModelError(
            [&] { authorizeBrowser(failed, authorizationEndpoint, {{"state", "fixture-state"}}, "", false, false, stop.get_token()); },
            "sign-in was cancelled", true);
        check(capture.view().find(authorizationEndpoint + '?') != std::string_view::npos, "CLI did not print a manual URL");
    }
    {
        CaptureStderr capture;
        std::optional<LoopbackRequest> pending;
        const auto result = authorizeBrowser(
            [&](const std::string& url) {
                pending.emplace(callbackPort(url), "/auth/callback?state=fixture-state&code=approved");
                return std::nullopt;
            },
            authorizationEndpoint, {{"state", "fixture-state"}}, "saved-client", false, false, {});
        check(pending->response().starts_with("HTTP/1.1 303") && result.code == "approved" && result.clientId == "saved-client",
              "An already queued callback did not complete sign-in");
        check(capture.view().find(authorizationEndpoint) == std::string_view::npos, "CLI printed a stale authorization URL");
    }
}
}

int main() {
    try {
        callbackChecks();
        openerFailure();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
