#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace flockv0 {
using Json = nlohmann::json;

std::string field(const Json&, std::string_view);
}
