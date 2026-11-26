#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace flockv0 {
struct ReadRequest {
    std::string environmentId, threadId, runId, itemId, cursor, view = "messages";
    std::size_t offset = 0;
    Json config = Json::object();
    std::string gatewayEnvironmentId;
    bool wholeSession = false;
};
struct Excerpt {
    std::string text, threadId, runId, itemId, nextCursor;
    std::size_t nextOffset = 0;
    bool more = false;
};
struct SessionPreview {
    std::string text;
    bool truncated = false;
};
struct SessionMetadata {
    std::string title, project, projectId, status, createdAt, updatedAt;
    std::vector<SessionPreview> messages;
    bool moreMessages = false;
};
struct SessionReadTimeout : std::runtime_error {
    SessionReadTimeout() : std::runtime_error("Session read deadline exceeded") {}
};
struct SessionAttachment {
    std::string id, name, mimeType;
    std::optional<std::string> content;
    bool truncated = false;
};
struct SessionEvent {
    std::string id, time, kind, status, title, text;
    std::vector<SessionAttachment> attachments;
    bool textTruncated = false, attachmentsTruncated = false;
};
struct SessionEventFilter {
    std::string kind, fromTime, toTime;
    unsigned limit = 10;
    std::optional<Id> afterPosition;
    std::function<bool(const SessionEvent&)> matchText;
};
struct SessionEventPage {
    std::vector<SessionEvent> items;
    std::optional<Id> nextPosition;
    bool truncated = false;
};
struct SessionCounts {
    Id messages = 0, runs = 0;
};
}
