#pragma once

#include "flock/web.hpp"

namespace flockv0 {
Json readChannelBridgeSettings(const std::filesystem::path&);
void validateChannelBridgeSettings(const Json&);
void configureManagedChannelBridges(Store&, const Json&, WebCallbacks&);
}
