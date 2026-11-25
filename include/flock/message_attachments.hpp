#pragma once

#include <cstddef>

namespace flockv0::message_attachments {
inline constexpr std::size_t maxFiles = 4;
inline constexpr std::size_t maxImageBytes = 2 * 1024 * 1024;
inline constexpr std::size_t maxAudioBytes = 64 * 1024;
inline constexpr unsigned maxImageEdge = 8192;
inline constexpr unsigned maxImagePixels = 4 * 1024 * 1024;

inline constexpr std::size_t maxEncodedFile = (maxImageBytes + 2) / 3 * 4;
inline constexpr std::size_t maxEncodedImages = maxFiles * maxEncodedFile;
inline constexpr std::size_t maxRequestBytes = maxEncodedImages + 132 * 1024;
}
