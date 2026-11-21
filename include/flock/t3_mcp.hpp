#pragma once
#include "flock/core.hpp"

namespace flockv0 {
// Environment URLs are configured independently of Bot settings. The directory
// is private and may contain credentials, never conversation state.
std::unique_ptr<T3> makeT3Mcp(const std::filesystem::path& credentialDirectory);
void loginT3(const std::filesystem::path& credentialDirectory,
             const std::string& environmentId, const std::string& mcpUrl,
             const std::function<void(const std::string&)>& showAuthorizationUrl);
} // namespace flockv0
