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

std::string_view trimProfileText(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = text.find_first_not_of(whitespace);
    if (first == text.npos) return {};
    return text.substr(first,text.find_last_not_of(whitespace) - first + 1);
}
}

AvatarImage::AvatarImage(std::string bytes)
    : format_(detectAvatarFormat(bytes)), bytes_(std::move(bytes)) {}

std::string AvatarImage::storageFilename(unsigned index) const {
    const auto base = std::string(format_.filename);
    return index == 0 ? base : std::to_string(index) + "-" + base;
}

bool AvatarImage::isStoredFilename(std::string_view name) {
    const auto digits = name.find_first_not_of("0123456789");
    if (digits > 0 && digits != name.npos && name.front() != '0' && name[digits] == '-')
        name.remove_prefix(digits + 1);
    return name == png.filename || name == jpeg.filename || name == gif.filename || name == webp.filename;
}

std::string validatedBotName(const Json& profile) {
    const auto found = profile.find("name");
    if (found == profile.end() || !found->is_string())
        throw BotProfileError("invalid_name","Bot name must be a string");
    const auto name = trimProfileText(found->get_ref<const std::string&>());
    if (name.empty() || name.size() > 128 ||
        std::ranges::count_if(name,[](unsigned char ch) { return (ch & 0xc0) != 0x80; }) > 32)
        throw BotProfileError("invalid_name","Bot name must be 1 to 32 characters");
    if (std::ranges::any_of(name,[](unsigned char ch) { return ch < 32 || ch == 127; }))
        throw BotProfileError("invalid_name","Bot name contains invalid characters");
    return std::string(name);
}

std::string validatedBotSoul(const Json& soul) {
    if (soul.is_string()) {
        const auto& text = soul.get_ref<const std::string&>();
        if (text.size() <= maxBotRoleBytes && text.find('\0') == text.npos)
            return std::string(trimProfileText(text));
    }
    throw BotProfileError("invalid_soul","Bot role must be text up to 8 KB without NUL characters");
}

std::size_t botRunSettingCount(const Json& profile) {
    return std::ranges::count_if(botRunSettingFields,[&](const char* key) { return profile.contains(key); });
}

bool validPlainId(std::string_view id) {
    return !id.empty() && id.size() <= 256 &&
        std::ranges::none_of(id,[](unsigned char ch) { return ch <= 32 || ch == 127; });
}

bool validReasoningEffort(std::string_view effort) {
    return !effort.empty() && effort.size() <= 32 &&
        effort.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == effort.npos;
}

const Json& fallbackReasoningEfforts() {
    static const auto levels = Json::array({"none","minimal","low","medium","high","xhigh","max"});
    return levels;
}

const Json& modelReasoningEfforts(const Json& catalog, std::string_view slug) {
    for (const auto& model : catalog) {
        if (model.value("slug","") != slug) continue;
        const auto levels = model.find("reasoningEfforts");
        if (levels != model.end() && levels->is_array()) return *levels;
        break;
    }
    return fallbackReasoningEfforts();
}

std::string validatedBotRunSetting(std::string_view key, const Json& value) {
    if (key == "reasoningEffort") {
        if (value.is_string()) {
            const auto& effort = value.get_ref<const std::string&>();
            if (effort.empty() || validReasoningEffort(effort)) return effort;
        }
        throw BotProfileError("invalid_reasoning_effort","Choose a reasoning effort from the list");
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        const auto id = trimProfileText(text);
        if (text.empty() || (text.size() <= 256 && validPlainId(id))) return std::string(id);
    }
    if (key == "model") throw BotProfileError("invalid_model","Choose a model from the list");
    throw BotProfileError("invalid_workspace","Use the environment and project IDs shown in T3");
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
