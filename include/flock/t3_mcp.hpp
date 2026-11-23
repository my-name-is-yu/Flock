#pragma once
#include "flock/core.hpp"

namespace flockv0 {
class ThreadSession;

// Environment URLs are configured independently of Bot settings. The directory
// is private and may contain credentials, never conversation state.
std::unique_ptr<T3> makeT3Mcp(const std::filesystem::path& credentialDirectory);
std::unique_ptr<ThreadSession> makeT3ThreadSession(const std::filesystem::path& credentialDirectory);
// Returns the saved connection's Operate permission.
bool loginT3(const std::filesystem::path& credentialDirectory,
             const std::string& environmentId, const std::string& mcpUrl,
             const std::function<void(const std::string&)>& showAuthorizationUrl, bool requireOperate = false);
}
