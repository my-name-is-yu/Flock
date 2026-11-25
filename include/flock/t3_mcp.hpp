#pragma once
#include "flock/core.hpp"
#include <stop_token>

namespace flockv0 {

// Environment URLs are configured independently of Bot settings. The directory
// is private and may contain credentials, never conversation state.
std::unique_ptr<T3> makeT3Mcp(const std::filesystem::path& credentialDirectory);

struct LocalT3Environment {
    std::string environmentId, mcpUrl;
};
std::optional<LocalT3Environment> discoverLocalT3();

void validateT3McpUrl(const std::string& mcpUrl);

struct T3SavedConnection {
    std::string label, mcpUrl;
    Seconds expiresAt;
    bool loopbackHttp;
};
std::optional<T3SavedConnection> savedT3Connection(const std::filesystem::path& credentialDirectory, const std::string& environmentId);

struct T3LoginResult {
    std::string environmentId;
    bool operate;
};

T3LoginResult loginT3(const std::filesystem::path& credentialDirectory, const std::optional<std::string>& expectedEnvironmentId,
                      const std::string& mcpUrl, const std::function<void(const std::string&)>& showAuthorizationUrl,
                      bool requireOperate = false,
                      const std::function<void(const std::string&, const std::function<void()>&)>& install = {}, std::stop_token stop = {});
}
