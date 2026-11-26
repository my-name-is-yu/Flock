#pragma once

#include "flock/person.hpp"

namespace flockv0 {
struct WebIdentityState {
    WebIdentityResolver resolve;
    bool liveInteractiveRoles = false;
};
}
