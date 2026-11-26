#pragma once

#include "flock/types.hpp"

namespace flockv0 {
struct RequestChanged : Conflict {
    RequestChanged() : Conflict("The request changed. Refresh the inbox and review the current request.") {}
};

struct DecisionUnavailable : Conflict {
    DecisionUnavailable() : Conflict("The request was already decided or closed.") {}
};

struct DecisionDenied : Conflict {
    DecisionDenied() : Conflict("You do not have permission to decide this request.") {}
};
}
