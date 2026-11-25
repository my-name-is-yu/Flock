#pragma once

#include "flock/json.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace flockv0 {

std::map<std::string, std::string> flockSkillNames(const Json& bots);
void exportFlockSkills(const std::filesystem::path& root, const Json& bots);

}
