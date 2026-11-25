#pragma once

#include "flock/workspace_tools.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace flockv0 {
class SecretRedactor;
struct Decision;

struct ToolContext {
    std::string bot;
    Id messageId;
    const BotTools& settings;
    bool hasWebEvidence;
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
