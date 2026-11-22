#pragma once
#include "flock/core.hpp"

namespace flockv0 {
class RootGate;
using T3RootOriginResolver = std::function<std::optional<std::string>(std::string_view environmentId)>;

// Environment URLs are configured independently of Bot settings. The directory
// is private and may contain credentials, never conversation state.
std::unique_ptr<T3> makeT3Mcp(const std::filesystem::path& credentialDirectory);
// The resolver is trusted local configuration and returns an origin or its /mcp URL.
std::unique_ptr<RootGate> makeT3RootGate(T3RootOriginResolver);
std::unique_ptr<RootGate> makeT3RootGate(const std::filesystem::path& credentialDirectory);
void loginT3(const std::filesystem::path& credentialDirectory,
             const std::string& environmentId, const std::string& mcpUrl,
             const std::function<void(const std::string&)>& showAuthorizationUrl);
}
