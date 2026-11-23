#pragma once

#include "storage.hpp"

#include <algorithm>
#include <limits>

namespace flockv0::conversation {
enum class PrincipalSource { gitWebhook = 0, trusted = 1, web = 2, channel = 3 };

struct QuestionAnchor {
    Id messageId, taskId;
};

struct Message {
    Id id = 0, replyTo = 0;
    std::string bot, text, channelScope;
    Json routing;
    Person admittedActor;
    PrincipalSource principalSource = PrincipalSource::gitWebhook;
    std::optional<QuestionAnchor> question;
};
}

namespace flockv0::conversation_storage {
inline constexpr auto messageSize = 16000;
inline constexpr std::size_t expandedContextBytes = 8192;
std::size_t boundExpandedContext(Json& turn);
void projectToolHistory(Json& turn, const Json& redactionRevision);
inline bool canRereadToolResult(std::string_view tool) {
    return !tool.empty() && tool != "read_tool_result" && tool != "expand_context";
}

inline bool hasText(std::string_view value) {
    return value.find_first_not_of(" \r\n\t") != std::string_view::npos;
}

struct ToolError : std::runtime_error {
    Json details;
    explicit ToolError(const std::string& message, Json extra = Json::object())
        : std::runtime_error(message), details(std::move(extra)) {}
};

inline std::string text(const Json& args, std::string_view key, std::size_t limit, bool required) {
    const auto value = args.find(key);
    if (value == args.end() || value->is_null()) {
        if (required) throw ToolError(std::string(key) + " is required");
        return {};
    }
    if (!value->is_string()) throw ToolError(std::string(key) + " must be a string");
    auto result = value->get<std::string>();
    if (!hasText(result)) {
        if (required) throw ToolError(std::string(key) + " must not be empty");
        return {};
    }
    if (result.size() > limit)
        throw ToolError(std::string(key) + " exceeds " + std::to_string(limit) + " bytes");
    return result;
}

inline Id identifier(const Json& args, std::string_view key, bool required, Id minimum = 1) {
    const auto value = args.find(key);
    if (value == args.end() || value->is_null()) {
        if (required) throw ToolError(std::string(key) + " is required");
        return 0;
    }
    if (!value->is_number_integer() || (value->is_number_unsigned() && value->get<std::uint64_t>() > std::numeric_limits<Id>::max()) ||
        value->get<Id>() < minimum)
        throw ToolError(std::string(key) + (minimum > 0 ? " must be a positive integer" : " must be a non-negative integer"));
    return value->get<Id>();
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
    const auto canonical = std::ranges::any_of(*aliases,[&](const Json& value) { return value == environment; });
    const auto id = canonical ? std::string(environment) : field(*aliases,environment);
    constexpr auto valid = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_:.";
    if (id.empty() || id.size() > 1024 || id.find_first_not_of(valid) != std::string::npos) return {};
    return id;
}
}
