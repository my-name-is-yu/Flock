#pragma once

#include "flock/types.hpp"
#include "flock/json.hpp"

#include <string_view>

namespace flockv0 {
struct GitPrFollowupEntry {
    std::string_view operationId, eventId;
    Id messageId;
};
}
