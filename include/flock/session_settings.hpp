#pragma once

#include "flock/json.hpp"

namespace flockv0 {
// Validate the documented Bot Session settings. Missing fields use T3 defaults.
Json normalizeSessionSettings(const Json& session);
}
