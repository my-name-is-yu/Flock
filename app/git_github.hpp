#pragma once
#include <memory>
#include <string>

namespace flockv0 {
class GitHost;
std::shared_ptr<GitHost> makeGitHubHost(std::string token);
void verifyGitHubToken(std::string token);
}
