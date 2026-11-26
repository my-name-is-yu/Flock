#pragma once

#include <cstddef>

namespace flockv0::limits::messages {
inline constexpr std::size_t maxBodyBytes = 16'000;
inline constexpr std::size_t maxWaitAnswerBytes = 8'000;
inline constexpr std::size_t maxAttachments = 4;
inline constexpr std::size_t maxImageBytes = 2 * 1024 * 1024;
inline constexpr std::size_t maxEncodedImageBytes = (maxImageBytes + 2) / 3 * 4;
inline constexpr std::size_t maxEncodedImagesBytes = maxAttachments * maxEncodedImageBytes;
static_assert(maxWaitAnswerBytes <= maxBodyBytes);
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
inline constexpr std::size_t maxReportBytes = 8'192;
inline constexpr std::size_t maxHistoryExcerptBytes = 8'192;
}

namespace flockv0::limits::conversation_context {
inline constexpr std::size_t maxBytes = 24'000;
inline constexpr std::size_t maxExpandedBytes = 8'192;
inline constexpr std::size_t emptyHistoryBytes = sizeof("[]") - 1;
static_assert(maxExpandedBytes + emptyHistoryBytes < maxBytes);
}

namespace flockv0::limits::memory {
inline constexpr std::size_t maxDocumentBytes = 8'192;
inline constexpr std::size_t maxLearnings = 3;
inline constexpr std::size_t maxLearningBytes = 300;
}

namespace flockv0::limits::model {
inline constexpr std::size_t maxDecisionReasonBytes = 4'000;
inline constexpr std::size_t maxToolArgumentBytes = 32'000;
inline constexpr int maxToolArgumentDepth = 20;
}
