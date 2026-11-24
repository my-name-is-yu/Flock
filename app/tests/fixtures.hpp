#pragma once

#include "../bots.hpp"

#include <chrono>
#include <fstream>
#include <unistd.h>

namespace flockv0::app::test {
class Sandbox {
public:
    Sandbox() {
        auto pattern = (std::filesystem::temp_directory_path() / "flock-app-XXXXXX").string();
        const char* made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Temporary directory setup failed");
        path = made;
    }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(path,error);
    }
    std::filesystem::path path;
};

inline void check(bool condition, std::string_view what) {
    if (!condition) throw std::runtime_error(std::string(what));
}

template <typename Error, typename Function>
void rejects(Function run, std::string_view what) {
    try { run(); }
    catch (const Error&) { return; }
    throw std::runtime_error(std::string(what));
}

inline Seconds utc(int year, unsigned month, unsigned day, int hour, int minute, int second = 0) {
    using namespace std::chrono;
    return duration_cast<seconds>((sys_days{std::chrono::year{year} / month / day} +
        hours{hour} + minutes{minute} + seconds{second}).time_since_epoch()).count();
}

inline void write(const std::filesystem::path& file, const std::string& data) {
    std::ofstream output(file);
    output << data;
    if (!output) throw std::runtime_error("Fixture write failed");
}

inline Json botConfig(std::string model = "fixture-model") {
    return {{"environmentId","env-canonical"},{"projectId","project-canonical"},{"modelProvider","chatgpt"},
        {"model",model},{"runTimeoutSeconds",999},
        {"session",{{"model","child-model"},{"providerInstanceId","child-provider"},{"runtimeMode","auto"},
            {"workspaceStrategy",{{"type","worktree"},{"baseRef","origin/main"},{"startFromOrigin",true}}}}},
        {"cron",Json::array()}};
}

inline void createBot(const std::filesystem::path& path, const Json& config, std::string soul, std::string memory) {
    std::filesystem::create_directories(path);
    write(path / "config.json",config.dump());
    write(path / "SOUL.md",soul);
    write(path / "MEMORY.md",memory);
}

void configuredSchedules();
void pinnedConversationDestinations();
}
