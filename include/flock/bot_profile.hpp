#pragma once

#include "flock/core.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>

namespace flockv0 {
inline std::optional<std::string> botProfileName(const Json& source) {
    if (!source.is_string()) return std::nullopt;
    const auto& value = source.get_ref<const std::string&>();
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::nullopt;
    const auto name = std::string_view(value).substr(first,value.find_last_not_of(" \t\r\n") - first + 1);
    if (name.size() > 128 || std::ranges::count_if(name,[](unsigned char ch) { return (ch & 0xC0) != 0x80; }) > 32 ||
        std::ranges::any_of(name,[](unsigned char ch) { return ch < 32 || ch == 127; })) return std::nullopt;
    return std::string(name);
}
}

namespace flockv0::avatar {
inline constexpr std::size_t maxImageBytes = 5 * 1024 * 1024;
inline constexpr std::array<std::string_view,7> allowedShapes = {
    "mug","sprout","controller","lightbulb","chat","watering-can","barbell"
};
inline constexpr std::array<std::string_view,8> allowedIcons = {
    "star","orbit","zap","compass","feather","hexagon","telescope","sparkles"
};
inline constexpr std::array<std::string_view,4> allowedRoles = {
    "reviewer","builder","researcher","custom"
};

inline bool validRole(const Json& role) {
    return role.is_string() &&
        std::ranges::find(allowedRoles,role.get_ref<const std::string&>()) != allowedRoles.end();
}

struct ImageFormat {
    std::string_view contentType, filename, signature;
};
inline constexpr ImageFormat imageFormats[] = {
    {"image/png","avatar.png","\x89PNG\r\n\x1a\n"},
    {"image/jpeg","avatar.jpg","\xff\xd8\xff"},
    {"image/webp","avatar.webp","RIFF"},
    {"image/gif","avatar.gif","GIF8"}
};

inline const ImageFormat* imageFormat(std::string_view bytes) {
    if (bytes.size() < 8 || bytes.size() > maxImageBytes) return nullptr;
    for (const auto& format : imageFormats) {
        if (!bytes.starts_with(format.signature)) continue;
        if (format.signature == "RIFF" && bytes.substr(8,4) != "WEBP") continue;
        if (format.signature == "GIF8" && bytes.substr(4,2) != "7a" && bytes.substr(4,2) != "9a") continue;
        return &format;
    }
    return nullptr;
}

inline bool validColor(std::string_view color) {
    return (color.size() == 4 || color.size() == 7) && color.front() == '#' &&
        std::all_of(color.begin()+1,color.end(),[](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

// The renderer validates the path and eye geometry before drawing the definition.
inline Json shapeDefinition(const Json& source) {
    if (!source.is_object() || source.value("version",Json()) != 1 ||
        !source.contains("id") || !source["id"].is_string() ||
        !source.contains("name") || !source["name"].is_string()) return nullptr;
    const auto& id = source["id"].get_ref<const std::string&>();
    if (id.empty() || id.size() > 64 ||
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
        return nullptr;
    const auto body = source.value("body",Json());
    const auto eyes = source.value("eyes",Json());
    if (!body.is_object() || !body.contains("path") || !body["path"].is_string() ||
        body["path"].get_ref<const std::string&>().size() > 12000 || !eyes.is_object()) return nullptr;
    auto coordinates = Json::object();
    for (const auto* key : {"x","y","spacing","scale"}) {
        if (!eyes.contains(key) || !eyes[key].is_number()) return nullptr;
        coordinates[key] = eyes[key];
    }
    return {{"version",1},{"id",id},{"name",source["name"]},{"body",{{"path",body["path"]}}},
            {"eyes",std::move(coordinates)}};
}

inline Json appearance(const Json& source) {
    if (!source.is_object()) return nullptr;
    const auto kind = source.value("kind",Json("shape"));
    if (!kind.is_string()) return nullptr;
    Json result = {{"kind",kind}};
    for (const auto* key : {"color","eyeColor"}) {
        if (!source.contains(key)) continue;
        if (!source[key].is_string() || !validColor(source[key].get_ref<const std::string&>())) return nullptr;
        result[key] = source[key];
    }
    if (kind == "shape") {
        if (source.contains("definition") || source.contains("body")) {
            auto definition = shapeDefinition(source.contains("definition") ? source["definition"] : source);
            if (definition.is_null()) return nullptr;
            result["definition"] = std::move(definition);
        }
        if (source.contains("example")) {
            if (!source["example"].is_string() ||
                std::ranges::find(allowedShapes,source["example"].get_ref<const std::string&>()) == allowedShapes.end())
                return nullptr;
            result["example"] = source["example"];
        }
        if (!result.contains("definition") && !result.contains("example")) return nullptr;
    } else if (kind == "icon") {
        if (!source.contains("icon") || !source["icon"].is_string() ||
            std::ranges::find(allowedIcons,source["icon"].get_ref<const std::string&>()) == allowedIcons.end())
            return nullptr;
        result["icon"] = source["icon"];
    } else if (kind == "upload") {
        if (source.contains("filename")) {
            if (!source["filename"].is_string() || source["filename"].get_ref<const std::string&>().size() > 256)
                return nullptr;
            result["filename"] = source["filename"];
        }
    } else return nullptr;
    return result;
}
}
