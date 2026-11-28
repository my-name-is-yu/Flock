#pragma once

#include <string>
#include <string_view>

namespace flockv0 {
// Encodes every byte except ASCII letters, digits, and the bytes in `keep`.
std::string percentEncode(std::string_view value, std::string_view keep = "-._~");
}
