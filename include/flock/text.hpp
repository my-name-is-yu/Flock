#pragma once

#include <string>
#include <string_view>

namespace flockv0 {
// Removes leading and trailing spaces, tabs, carriage returns and newlines.
inline std::string trimAscii(std::string_view value) {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == value.npos) return {};
    return std::string(value.substr(first, value.find_last_not_of(whitespace) - first + 1));
}
}
