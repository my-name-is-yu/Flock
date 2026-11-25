#pragma once

#include "flock/json.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace flockv0::detail {

inline Json decodeRpcResponse(std::string_view body, std::string_view contentType, std::int64_t id, Json (*parse)(std::string_view)) {
    const auto matches = [id](const Json& message) {
        const auto field = message.find("id");
        return field != message.end() && *field == id;
    };
    if (!contentType.starts_with("text/event-stream")) {
        auto message = parse(body);
        if (matches(message)) return message;
        return nullptr;
    }

    std::string event;
    while (!body.empty()) {
        const auto end = body.find('\n');
        auto line = body.substr(0, end);
        body.remove_prefix(end == body.npos ? body.size() : end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.starts_with("data:")) {
            line.remove_prefix(5);
            if (line.starts_with(' ')) line.remove_prefix(1);
            event += line;
            event += '\n';
        } else if (line.empty() && !event.empty()) {
            auto message = parse(event);
            if (matches(message)) return message;
            event.clear();
        }
    }
    return nullptr;
}

}
