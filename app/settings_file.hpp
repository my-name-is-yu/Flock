#pragma once

#include "flock/json.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string_view>

namespace flockv0::app {
Json readSettingsFile(const std::filesystem::path&, Json fallback = Json::object());
void writeSettingsFile(const std::filesystem::path&, const Json&);
void writePrivateFile(const std::filesystem::path&, std::string_view);
}
