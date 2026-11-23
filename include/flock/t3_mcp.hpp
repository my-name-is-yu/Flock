#pragma once
#include "flock/core.hpp"
#include <stop_token>

namespace flockv0 {

// Environment URLs are configured independently of Bot settings. The directory
// is private and may contain credentials, never conversation state.
std::unique_ptr<T3> makeT3Mcp(const std::filesystem::path& credentialDirectory);
void validateT3ConnectTarget(const std::string& environmentId, const std::string& mcpUrl);
void validateT3McpUrl(const std::string& mcpUrl);

// Use the authenticated environment identity when a caller has only its connection URL.
bool loginT3Environment(const std::filesystem::path& credentialDirectory,
             const std::optional<std::string>& expectedEnvironmentId, const std::string& mcpUrl,
             const std::function<void(const std::string&)>& showAuthorizationUrl, bool requireOperate,
             const std::function<void(const std::string&, const std::function<void()>&)>& install,
             std::stop_token stop = {});

// Returns the saved connection's Operate permission.
bool loginT3(const std::filesystem::path& credentialDirectory,
             const std::string& environmentId, const std::string& mcpUrl,
             const std::function<void(const std::string&)>& showAuthorizationUrl, bool requireOperate = false,
             const std::function<void(const std::function<void()>&)>& install = {}, std::stop_token stop = {});
}
