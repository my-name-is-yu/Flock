#pragma once

#include <cstddef>
#include <string_view>

namespace flockv0::limits::messages {
inline constexpr std::size_t maxBodyBytes = 16'000;
inline constexpr std::size_t maxWaitAnswerBytes = 8'000;
inline constexpr std::size_t maxAttachments = 8;
inline constexpr std::size_t maxImageBytes = 10 * 1024 * 1024;
inline constexpr std::size_t maxFileBytes = 50 * 1024 * 1024;
inline constexpr std::size_t maxEncodedFilesBytes = maxAttachments * ((maxFileBytes + 2) / 3 * 4);
inline constexpr std::size_t maxEncodedImageBytes = (maxImageBytes + 2) / 3 * 4;
inline constexpr std::size_t maxEncodedImagesBytes = maxAttachments * maxEncodedImageBytes;
static_assert(maxWaitAnswerBytes <= maxBodyBytes);
}

namespace flockv0::limits::messages {
inline bool isAttachmentImage(std::string_view mime) {
    return mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" || mime == "image/webp";
}
}

namespace flockv0::limits::mcp {
inline constexpr std::size_t maxRequestTextBytes = 8'192;
static_assert(maxRequestTextBytes <= messages::maxBodyBytes);
}

namespace flockv0::limits::session_prompt {
inline constexpr std::size_t maxBytes = 120'000;
inline constexpr std::size_t maxQuoteTextBytes = 8'192;
inline constexpr std::size_t maxDecisionTextBytes = 8'000;
inline constexpr std::size_t framingReserveBytes = 512;
inline constexpr std::size_t minActionIdReserveBytes = 64;
inline constexpr std::size_t instructionReserveBytes = maxDecisionTextBytes + framingReserveBytes;
static_assert(instructionReserveBytes + maxDecisionTextBytes + minActionIdReserveBytes + maxQuoteTextBytes <= maxBytes);

inline constexpr std::size_t pendingBatchJsonThresholdBytes = 64'000;
}

namespace flockv0::limits::session_evidence {
inline constexpr std::size_t maxReportBytes = 65'536;
inline constexpr std::size_t maxHistoryExcerptBytes = 32'768;
}

namespace flockv0::limits::conversation_context {
inline constexpr std::size_t maxBytes = 192'000;
inline constexpr std::size_t maxExpandedBytes = 65'536;
inline constexpr std::size_t maxEntries = 64;
inline constexpr std::size_t maxUserTextBytes = messages::maxBodyBytes;
inline constexpr std::size_t maxReplyTextBytes = 8'000;
inline constexpr std::size_t maxTaskTextBytes = 4'000;
inline constexpr std::size_t emptyHistoryBytes = sizeof("[]") - 1;
static_assert(maxExpandedBytes + emptyHistoryBytes < maxBytes);
}

namespace flockv0::limits::memory {
inline constexpr std::size_t maxDocumentBytes = 32'768;
inline constexpr std::size_t maxLearnings = 12;
inline constexpr std::size_t maxLearningBytes = 1'000;
}

namespace flockv0::limits::model {
inline constexpr std::size_t maxDecisionReasonBytes = 4'000;
inline constexpr std::size_t maxToolArgumentBytes = 262'144;
inline constexpr int maxToolArgumentDepth = 20;
}
