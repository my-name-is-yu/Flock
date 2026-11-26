#pragma once

#include "flock/types.hpp"

namespace flockv0 {
enum class SessionExit { removed, left };
struct SessionStop {
    Id participation = 0, cutoff = 0;
};
}
