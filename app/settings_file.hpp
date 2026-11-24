#pragma once

#include "flock/core.hpp"

#include <filesystem>

namespace flockv0::app {
Json readSettingsFile(const std::filesystem::path&, Json fallback = Json::object());
void writeSettingsFile(const std::filesystem::path&, const Json&);
void writePrivateFile(const std::filesystem::path&, std::string_view);
}
