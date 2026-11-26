#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace flockv0 {
using SoulLoader = std::function<std::string(std::string_view)>;
}
