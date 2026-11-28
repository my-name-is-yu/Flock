#pragma once

#include <cstdint>
#include <string>

namespace flockv0 {
struct TaskAttachmentRef {
    std::int64_t messageId;
    std::string attachmentId;
    bool operator==(const TaskAttachmentRef&) const = default;
};

struct AttachmentFile {
    std::string name, mimeType, bytes;
};
}
