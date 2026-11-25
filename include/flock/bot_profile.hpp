#pragma once
#include "flock/core.hpp"

namespace flockv0 {
struct BotProfileError : Conflict {
    BotProfileError(std::string code, const char* message) : Conflict(message), code(std::move(code)) {}
    std::string code;
};

inline constexpr std::size_t maxBotRoleBytes = 8192;
inline constexpr const char* botRunSettingFields[] = {"environmentId", "projectId", "model", "reasoningEffort", "modelCallLimit"};

std::string validatedBotName(const Json& profile);
std::string validatedBotSoul(const Json& soul);
bool validatedBotFixPullRequests(const Json& value);
std::size_t botRunSettingCount(const Json& profile);
bool validPlainId(std::string_view id);
bool validReasoningEffort(std::string_view effort);
const Json& fallbackReasoningEfforts();
const Json& modelReasoningEfforts(const Json& catalog, std::string_view slug);
Json validatedBotRunSetting(std::string_view key, const Json& value);
bool validBotRole(std::string_view role);
Json validatedAvatarStyle(const Json& profile);

struct AvatarFormat {
    std::string_view filename, contentType;
};

class AvatarImage {
public:
    static constexpr std::size_t maxBytes = 5 * 1024 * 1024;
    explicit AvatarImage(std::string bytes);
    std::string storageFilename(unsigned index) const;
    static bool isStoredFilename(std::string_view name);
    const AvatarFormat& format() const { return format_; }
    const std::string& bytes() const { return bytes_; }
private:
    AvatarFormat format_;
    std::string bytes_;
};
}
