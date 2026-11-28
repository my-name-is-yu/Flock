#pragma once

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace flockv0::live_test {
inline constexpr auto chatGPTSetup = "Export FLOCK_HOME with the test state directory and FLOCK_LIVE_MODEL with a ChatGPT model slug.\n"
                                     "Run flock --home \"$FLOCK_HOME\" model login and grant ChatGPT plan usage.\n"
                                     "List model slugs with flock --home \"$FLOCK_HOME\" model models.\n";
inline constexpr auto t3Setup =
    "Export FLOCK_HOME with the test state directory, FLOCK_LIVE_T3_ENVIRONMENT with the canonical T3 environment ID,\n"
    "and FLOCK_LIVE_T3_PROJECT with the canonical ID of a test project on that environment.\n"
    "Run flock_live_t3 login \"$FLOCK_HOME/credentials/t3\" \"$FLOCK_LIVE_T3_ENVIRONMENT\" <MCP URL>\n"
    "using the T3 connection URL ending in /mcp. Open the printed URL to grant T3 Operate access.\n";

inline std::string environment(const char* name) {
    const auto* value = std::getenv(name);
    return value ? value : "";
}

inline int skip(std::string_view reason, std::string_view setup) {
    std::cout << "SKIP: " << reason << '\n' << setup;
    return 77;
}

inline bool hasChatGPTProfile(const std::filesystem::path& directory) {
    return std::filesystem::exists(std::filesystem::symlink_status(directory / "profile.json"));
}

inline bool hasT3Credentials(const std::filesystem::path& directory) {
    const auto status = std::filesystem::symlink_status(directory);
    if (!std::filesystem::exists(status)) return false;
    if (!std::filesystem::is_directory(status)) throw std::runtime_error("T3 credentials require a private directory");
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().starts_with("t3-") && entry.path().extension() == ".json") return true;
    }
    return false;
}
}
