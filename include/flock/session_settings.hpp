#pragma once

#include "flock/core.hpp"

namespace flockv0 {
// Validate the documented Bot Session settings. Missing fields use T3 defaults.
Json normalizeSessionSettings(const Json& session);
}
