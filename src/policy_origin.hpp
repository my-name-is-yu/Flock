#pragma once

#include "flock/types.hpp"

#include <string>

namespace flockv0 {
struct PolicyOrigin {
    std::string bot, scope, requester;
    Id anchor = 0, turn = 0, task = 0, revision = 0, generation = 0;
    bool background = false;
};
}
