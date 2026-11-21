#pragma once

#include "storage.hpp"

#include <algorithm>

namespace flockv0::conversation_storage {
inline constexpr auto messageSize = 16000;

inline bool hasText(std::string_view value) {
    return value.find_first_not_of(" \r\n\t") != std::string_view::npos;
}

inline Id integer(const Json& data, std::string_view key) {
    const auto value = data.find(key);
    return value != data.end() && value->is_number_integer() ? value->get<Id>() : 0;
}

inline std::string verifiedEnvironment(const Json& task, const Json& routing, std::string_view environment) {
    if (environment.empty()) return {};
    if (environment == field(task,"environmentId") || environment == field(routing,"environmentId"))
        return std::string(environment);
    const auto& config = task["projectId"] == routing["projectId"] ? routing.at("config") : task.at("config");
    if (!config.is_object()) return {};
    const auto aliases = config.find("conversationEnvironments");
    if (aliases == config.end() || !aliases->is_object()) return {};
    auto id = field(*aliases,environment);
    if (id.empty() && std::ranges::any_of(*aliases,[&](const Json& value) { return value == environment; }))
        id = environment;
    constexpr auto valid = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_:.";
    if (id.empty() || id.size() > 1024 || id.find_first_not_of(valid) != std::string::npos) return {};
    return id;
}
}
