#pragma once
#include "flock/git_host.hpp"

namespace flockv0 {
std::shared_ptr<GitHost> makeGitHubHost(std::string token);
void verifyGitHubToken(std::string token);
}
