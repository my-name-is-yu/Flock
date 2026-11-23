#pragma once

#include <string>
#include <string_view>

namespace flockv0::markdown {
std::string toHtml(std::string_view source);
std::string toSlack(std::string_view source);
std::string toPlain(std::string_view source);
}
