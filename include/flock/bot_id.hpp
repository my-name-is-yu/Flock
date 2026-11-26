#pragma once

#include <string>
#include <string_view>

namespace flockv0 {
bool validBotId(std::string_view);
std::string botName(std::string_view);
}
