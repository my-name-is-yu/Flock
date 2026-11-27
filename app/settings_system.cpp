#include "settings_system.hpp"
#include "settings_file.hpp"
#include "t3_setup.hpp"
#include "flock/json.hpp"
#include "flock/t3_connection.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <netdb.h>
#include <netinet/in.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace flockv0::app {
namespace {

unsigned portSetting(const Json& value) {
    if (!value.is_number_integer() || value < 1 || value > 65535) throw Conflict("Enter a port from 1 to 65535.");
    return value.get<unsigned>();
}

constexpr auto defaultSessionRead = "on_demand";

std::string sessionReadSetting(const Json& value) {
    if (value != "off" && value != "on_demand" && value != "prefill") throw Conflict("Choose how Flock reads T3 Sessions.");
    return value.get<std::string>();
}

bool reachableFromOtherMachines(const std::string& address) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(address.c_str(), nullptr, &hints, &found) != 0) return true;
    bool reachable = true;
    for (auto* entry = found; entry; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET) {
            const auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr.s_addr);
            reachable = reachable && ip != INADDR_ANY && (ip >> 24) != IN_LOOPBACKNET;
        } else if (entry->ai_family == AF_INET6) {
            const auto& ip = reinterpret_cast<const sockaddr_in6*>(entry->ai_addr)->sin6_addr;
            reachable = reachable && !IN6_IS_ADDR_UNSPECIFIED(&ip) && !IN6_IS_ADDR_LOOPBACK(&ip) && !IN6_IS_ADDR_V4MAPPED(&ip);
        }
    }
    freeaddrinfo(found);
    return reachable;
}

void validateRemoteAddress(const Json& value) {
    if (value.is_null()) return;
    constexpr auto invalid = "Enter this computer's address on a private network, such as its Tailscale IP or name.";
    if (!value.is_string()) throw Conflict(invalid);
    const auto& address = value.get_ref<const std::string&>();
    if (address.empty() || address.size() > 253 ||
        address.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-:") != address.npos)
        throw Conflict(invalid);
    if (address == "localhost" || !reachableFromOtherMachines(address)) throw Conflict(invalid);
}

void validate(const Json& settings) {
    if (!settings.is_object()) throw Conflict("Invalid saved system settings.");
    for (const auto& [key, value] : settings.items()) {
        if (key == "port" || key == "previousPort") {
            if (!value.is_null()) (void)portSetting(value);
        } else if (key == "sessionRead") {
            if (!value.is_null()) (void)sessionReadSetting(value);
        } else if (key == "remoteAddress") {
            validateRemoteAddress(value);
        } else
            throw Conflict("Unknown system setting.");
    }
}

}

SettingsSystem::SettingsSystem(const std::filesystem::path& home, std::optional<unsigned> cliPort,
                               std::optional<std::string> cliSessionRead)
    : file_(home / "system-settings.json"), cliPort_(cliPort), cliSessionRead_(cliSessionRead.has_value()),
      saved_(readSettingsFile(file_)) {
    validate(saved_);
    startupPortSetting_ = saved_.value("port", Json(nullptr));
    const auto mode = saved_.value("sessionRead", Json(nullptr));
    activeSessionRead_ = cliSessionRead.value_or(mode.is_null() ? defaultSessionRead : mode.get<std::string>());
    const auto remoteAddress = saved_.value("remoteAddress", Json(nullptr));
    if (!remoteAddress.is_null()) activeRemoteAddress_ = remoteAddress.get<std::string>();
}

unsigned SettingsSystem::startupPort(unsigned t3Port) const {
    std::lock_guard lock(mutex_);
    if (cliPort_) {
        if (t3Port && *cliPort_ != t3Port)
            throw Conflict("Flock Session tools use the saved Web port " + std::to_string(t3Port) +
                           ". Omit --port or change the port in Flock settings.");
        return *cliPort_;
    }
    const auto port = saved_.value("port", Json(nullptr));
    return port.is_null() ? t3Port : port.get<unsigned>();
}

unsigned SettingsSystem::t3CliPort(unsigned requestedPort, unsigned t3Port) const {
    std::lock_guard lock(mutex_);
    const auto setting = saved_.value("port", Json(nullptr));
    const auto preferred = setting.is_null() ? 0u : setting.get<unsigned>();
    if (preferred && requestedPort && requestedPort != preferred)
        throw Conflict("Flock System settings select Web port " + std::to_string(preferred) +
                       ". Change the port in Flock settings before connecting T3.");
    if (requestedPort) return requestedPort;
    if (preferred) return preferred;
    return t3Port ? t3Port : 4173;
}

std::optional<unsigned> SettingsSystem::recoveryPort(unsigned t3Port) const {
    std::lock_guard lock(mutex_);
    const auto port = saved_.value("port", Json(nullptr));
    if (cliPort_ || port.is_null() || port == t3Port) return {};
    if (t3Port) return t3Port;
    const auto previous = saved_.value("previousPort", Json(nullptr));
    return previous.is_null() || previous == port ? 0 : previous.get<unsigned>();
}

void SettingsSystem::listenerStarted(unsigned port) {
    std::lock_guard lock(mutex_);
    activePort_ = port;
}

Json SettingsSystem::viewLocked(unsigned t3Port) const {
    const auto activePort = activePort_ ? Json(*activePort_) : Json(nullptr);
    const auto savedPort = saved_.value("port", Json(nullptr));
    const auto savedRead = saved_.value("sessionRead", Json(nullptr));
    const auto nextPort = cliPort_.value_or(savedPort.is_null() ? t3Port : savedPort.get<unsigned>());
    const auto nextRead = cliSessionRead_ ? Json(activeSessionRead_) : savedRead.is_null() ? Json(defaultSessionRead) : savedRead;
    const bool pendingPort = !cliPort_ && (nextPort ? nextPort != activePort_.value_or(0) : !startupPortSetting_.is_null());
    const bool pendingRead = nextRead != activeSessionRead_;
    const auto activeRemote = activeRemoteAddress_ ? Json(*activeRemoteAddress_) : Json(nullptr);
    const auto nextRemote = saved_.value("remoteAddress", Json(nullptr));
    return {{"active", {{"port", activePort}, {"sessionRead", activeSessionRead_}, {"remoteAddress", activeRemote}}},
            {"saved", {{"port", savedPort}, {"sessionRead", savedRead}, {"remoteAddress", nextRemote}}},
            {"next", {{"port", nextPort ? Json(nextPort) : Json(nullptr)}, {"sessionRead", nextRead}, {"remoteAddress", nextRemote}}},
            {"overrides", {{"port", cliPort_.has_value()}, {"sessionRead", cliSessionRead_}, {"remoteAddress", false}}},
            {"pending", {{"port", pendingPort}, {"sessionRead", pendingRead}, {"remoteAddress", nextRemote != activeRemote}}}};
}

Json SettingsSystem::view(unsigned t3Port) const {
    std::lock_guard lock(mutex_);
    return viewLocked(t3Port);
}

Json SettingsSystem::change(const Json& patch, unsigned t3Port) {
    if (!patch.is_object() || patch.empty()) throw Conflict("Select a system setting to change.");
    std::lock_guard lock(mutex_);
    auto next = saved_;
    for (const auto& [key, value] : patch.items()) {
        if (key == "port") {
            if (cliPort_) throw Conflict("The startup command sets the current port.");
            if (!value.is_null()) (void)portSetting(value);
            if (next.value("port", Json(nullptr)) != value && activePort_) next["previousPort"] = *activePort_;
        } else if (key == "sessionRead") {
            if (cliSessionRead_) throw Conflict("The startup command sets how Flock reads T3 Sessions.");
            if (!value.is_null()) (void)sessionReadSetting(value);
        } else if (key == "remoteAddress") {
            validateRemoteAddress(value);
        } else
            throw Conflict("Unknown system setting.");
        if (value.is_null())
            next.erase(key);
        else
            next[key] = value;
    }
    if (next != saved_) {
        writeSettingsFile(file_, next);
        saved_ = std::move(next);
    }
    return viewLocked(t3Port);
}

void rebindT3Port(Store& store, unsigned port) {
    T3Connections connections(store);
    const auto previous = connections.port();
    if (!previous || previous == port) return;
    const T3SetupLock setup;
    configureT3Providers(
        "http://127.0.0.1:" + std::to_string(port) + "/mcp/flock",
        [&](const InstallT3Provider& install) { connections.rebindPort(port, install); },
        [&](std::string_view token) { return connections.ownsBearer(token); });
}

}
