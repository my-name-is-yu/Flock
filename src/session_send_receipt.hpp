#pragma once

#include "flock/run_ref.hpp"
#include "flock/session_ref.hpp"
#include "flock/types.hpp"
#include <string>

namespace flockv0 {
struct SessionSendOutcome {
    enum class Status { Started, Queued, Unsent, NotAccepted, Unknown } status = Status::Unknown;
    RunRef run;
    std::string detail;
};

struct SessionSendReceipt {
    std::string bot;
    SessionTarget target;
    std::string title;
    SessionSendOutcome outcome;
    int step = -1;
    Id taskId = 0;
    std::string actionId, requestId;
    bool notify = true;
    bool hasAttachments = false;
};
}
