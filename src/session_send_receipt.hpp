#pragma once

#include "flock/session_ref.hpp"
#include "flock/t3_command.hpp"
#include "flock/types.hpp"
#include <string>

namespace flockv0 {
struct SessionSendReceipt {
    std::string bot;
    SessionTarget target;
    std::string title;
    Delivery delivery;
    int step = -1;
    Id taskId = 0;
    std::string actionId, requestId;
    bool notify = true;
};
}
