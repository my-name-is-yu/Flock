#include "flock/bot_profile.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

namespace flockv0 {
namespace {
constexpr AvatarFormat png{"avatar.png","image/png"};
constexpr AvatarFormat jpeg{"avatar.jpg","image/jpeg"};
constexpr AvatarFormat gif{"avatar.gif","image/gif"};
constexpr AvatarFormat webp{"avatar.webp","image/webp"};

AvatarFormat detectAvatarFormat(std::string_view bytes) {
    if (bytes.size() >= 8 && bytes.size() <= AvatarImage::maxBytes) {
        if (bytes.starts_with("\x89PNG\r\n\x1a\n")) return png;
        if (bytes.starts_with("\xff\xd8\xff")) return jpeg;
        if (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a")) return gif;
        if (bytes.starts_with("RIFF") && bytes.substr(8).starts_with("WEBP")) return webp;
    }
    throw BotProfileError("invalid_image","Choose a PNG, JPEG, WebP or GIF up to 5 MB");
}

constexpr std::string_view shapes[] = {
    "mug", "sprout", "controller", "lightbulb", "chat", "watering-can", "barbell"
};
constexpr std::string_view icons[] = {
    "star", "orbit", "zap", "compass", "feather", "hexagon", "telescope", "sparkles"
};

bool validHexColor(std::string_view color) {
    return (color.size() == 4 || color.size() == 7) && color.front() == '#' &&
        std::all_of(color.begin() + 1,color.end(),[](unsigned char ch) { return std::isxdigit(ch); });
}
}

AvatarImage::AvatarImage(std::string bytes)
    : format_(detectAvatarFormat(bytes)), bytes_(std::move(bytes)) {}

bool AvatarImage::isStoredFilename(std::string_view name) {
    return name == png.filename || name == jpeg.filename || name == gif.filename || name == webp.filename;
}

std::string validatedBotName(const Json& profile) {
    const auto found = profile.find("name");
    if (found == profile.end() || !found->is_string())
        throw BotProfileError("invalid_name","Bot name must be a string");
    const std::string_view raw = found->get_ref<const std::string&>();
    const auto first = raw.find_first_not_of(" \t\r\n");
    const auto name = first == raw.npos ? std::string_view{}
        : raw.substr(first,raw.find_last_not_of(" \t\r\n") - first + 1);
    if (name.empty() || name.size() > 128 ||
        std::ranges::count_if(name,[](unsigned char ch) { return (ch & 0xc0) != 0x80; }) > 32)
        throw BotProfileError("invalid_name","Bot name must be 1 to 32 characters");
    if (std::ranges::any_of(name,[](unsigned char ch) { return ch < 32 || ch == 127; }))
        throw BotProfileError("invalid_name","Bot name contains invalid characters");
    return std::string(name);
}

bool validBotRole(std::string_view role) {
    return role == "reviewer" || role == "builder" || role == "researcher" || role == "custom";
}

Json validatedAvatarStyle(const Json& profile) {
    const auto avatar = profile.find("avatar");
    if (avatar == profile.end() || !avatar->is_object())
        throw BotProfileError("invalid_avatar","Avatar must be an object");
    const auto kind = avatar->find("kind");
    if (kind == avatar->end() || !kind->is_string())
        throw BotProfileError("invalid_avatar","Avatar kind is required");
    Json style = {{"kind",*kind},{"color","#69c9ac"}};
    if (const auto color = avatar->find("color"); color != avatar->end()) {
        if (!color->is_string() || !validHexColor(color->get_ref<const std::string&>()))
            throw BotProfileError("invalid_avatar","Avatar color must be a hex color");
        style["color"] = *color;
    }
    if (*kind == "shape") {
        const auto example = avatar->find("example");
        if (example == avatar->end() || !example->is_string() ||
            std::ranges::find(shapes,example->get_ref<const std::string&>()) == std::end(shapes))
            throw BotProfileError("invalid_avatar","Unsupported avatar shape");
        style["example"] = *example;
    } else if (*kind == "icon") {
        const auto icon = avatar->find("icon");
        if (icon == avatar->end() || !icon->is_string() ||
            std::ranges::find(icons,icon->get_ref<const std::string&>()) == std::end(icons))
            throw BotProfileError("invalid_avatar","Unsupported avatar icon");
        style["icon"] = *icon;
    } else if (*kind != "upload") {
        throw BotProfileError("invalid_avatar","Unsupported avatar kind");
    }
    return style;
}
}
