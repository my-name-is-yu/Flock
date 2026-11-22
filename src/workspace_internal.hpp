#pragma once

#include "flock/workspace_tools.hpp"

#include <thread>
#include <atomic>

namespace flockv0 {
struct ToolContext {
    std::string bot;
    Id messageId;
    const BotTools& settings;
};

struct WorkspaceJob {
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
    std::jthread worker;
};

std::filesystem::path relativeToolPath(std::string_view);
std::string toolText(const Json&, std::string_view key, std::size_t limit, bool allowEmpty = false);
Id toolId(const Json&, std::string_view key);
Json toolFailure(std::string_view);
Json readToolFile(const std::filesystem::path& root, const std::filesystem::path& relative);
std::string webHost(std::string_view url);
}
