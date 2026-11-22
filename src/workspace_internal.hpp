#pragma once

#include "flock/workspace_tools.hpp"

#include <array>
#include <atomic>
#include <thread>
#include <variant>

namespace flockv0 {
class SecretRedactor;

struct ToolContext {
    std::string bot;
    Id messageId;
    const BotTools& settings;
    bool hasWebEvidence;
};

struct WorkspaceOutput {
    std::array<std::string,2> text;
    bool truncated = false;
    bool readAvailable(int fd, unsigned channel);
};

struct WorkspaceOutcome {
    WorkspaceOutput capture;
    std::optional<int> exitCode; // Missing when the command outcome is unknown.
    bool incomplete = false;
};

struct WorkspaceJob {
    std::atomic<bool> stop{false};
    // Published by the worker under the Store mutex and held until the terminal write succeeds.
    std::variant<std::monostate,WorkspaceOutcome,std::exception_ptr> completion;
    std::jthread worker;
};

std::filesystem::path relativeToolPath(std::string_view);
int openExactChildDirectory(int parent, const std::filesystem::path& name);
std::string toolText(const Json&, std::string_view key, std::size_t limit, bool allowEmpty = false);
Id toolId(const Json&, std::string_view key);
Json toolFailure(std::string_view);
bool allowedAfterWebEvidence(const Decision&);
Json webEvidenceActionDenied();
bool pathContains(const std::filesystem::path& root, const std::filesystem::path& child);
Json readToolFile(const std::filesystem::path& root, const std::filesystem::path& relative, const SecretRedactor&);
std::string webHost(std::string_view url);
}
