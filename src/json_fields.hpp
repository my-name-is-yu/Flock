#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <string_view>

namespace flockv0 {
Id requiredInteger(const Json&, std::string_view key, Id minimum = 1);
}
