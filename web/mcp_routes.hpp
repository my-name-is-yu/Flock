#pragma once
#include "flock/web.hpp"
#include <httplib.h>

namespace flockv0::web {
inline constexpr auto sessionMcpPath = "/mcp/flock";
inline constexpr auto externalMcpPath = "/mcp/flock/external";
void mcpRoutes(httplib::Server&, Store&, const WebCallbacks&);
}
