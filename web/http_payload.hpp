#pragma once

#include <cstddef>

namespace flockv0::web {
inline constexpr std::size_t maxStandardPayload = 128 * 1024;
inline constexpr std::size_t maxUploadPayload = 7 * 1024 * 1024 + maxStandardPayload;
}
