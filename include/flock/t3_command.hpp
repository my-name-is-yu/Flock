#pragma once

#include "flock/command.hpp"
#include "flock/json.hpp"
#include "flock/session_ref.hpp"
#include "flock/task_attachment.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace flockv0 {
struct Command : CommandPayload {
    Json config = Json::object();
    std::string authGeneration{}, ownerBotId{};
    std::optional<SessionOrigin> sessionOrigin{};
    std::vector<TaskAttachment> attachments{};
    std::size_t attachmentCount = 0;
};
}
