#pragma once

#include "flock/workspace_tools.hpp"

#include <thread>
#include <atomic>

namespace flockv0 {
class SecretRedactor;

struct ToolContext {
    std::string bot;
    Id messageId;
    const BotTools& settings;
    bool hasWebEvidence;
};

struct WorkspaceJob {
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
    std::jthread worker;
};

std::filesystem::path relativeToolPath(std::string_view);
int openExactChildDirectory(int parent, const std::filesystem::path& name);
std::string toolText(const Json&, std::string_view key, std::size_t limit, bool allowEmpty = false);
Id toolId(const Json&, std::string_view key);
Json toolFailure(std::string_view);
bool allowedAfterWebEvidence(std::string_view);
Json webEvidenceActionDenied();
bool pathContains(const std::filesystem::path& root, const std::filesystem::path& child);
Json readToolFile(const std::filesystem::path& root, const std::filesystem::path& relative, const SecretRedactor&);
std::string webHost(std::string_view url);
}
