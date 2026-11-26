#pragma once

#include "flock/json.hpp"

#include <string>
#include <string_view>

namespace flockv0 {
std::string field(const Json&, std::string_view);
}
