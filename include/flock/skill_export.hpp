#pragma once

#include "flock/core.hpp"
#include <map>

namespace flockv0 {

std::map<std::string, std::string> flockSkillNames(const Json& bots);
void exportFlockSkills(const std::filesystem::path& root, const Json& bots);

}
