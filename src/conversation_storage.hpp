#pragma once

#include "flock/types.hpp"
#include "flock/person.hpp"
#include "flock/json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace flockv0::conversation {
enum class PrincipalSource { gitWebhook = 0, trusted = 1, web = 2, channel = 3, session = 4 };

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
    // Set when the message came from a Bot or a Session and not from the person.
    bool botOrigin = false;
};

// Whether a turn for this message may use the tool. Changes that need the person's own request are refused to other origins.
bool allowsTurnTool(const Message&, std::string_view tool);
}

namespace flockv0::conversation_storage {
void projectToolHistory(Json& turn, const Json& redactionRevision);
inline bool japanese(std::string_view text) {
    return std::ranges::any_of(text, [](unsigned char byte) { return byte == 0xe3 || (byte >= 0xe4 && byte <= 0xe9); });
}

inline bool canRereadToolResult(std::string_view tool) {
    return !tool.empty() && tool != "read_tool_result" && tool != "expand_context";
}

inline bool hasText(std::string_view value) {
    return value.find_first_not_of(" \r\n\t") != std::string_view::npos;
}

struct ToolError : std::runtime_error {
    Json details;
    explicit ToolError(const std::string& message, Json extra = Json::object()) : std::runtime_error(message), details(std::move(extra)) {}
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
    if (result.size() > limit) throw ToolError(std::string(key) + " exceeds " + std::to_string(limit) + " bytes");
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

}
