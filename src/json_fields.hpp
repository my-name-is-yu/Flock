#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <string>
#include <string_view>

namespace flockv0 {
std::string field(const Json&, std::string_view);
Id requiredInteger(const Json&, std::string_view key, Id minimum = 1);
}
