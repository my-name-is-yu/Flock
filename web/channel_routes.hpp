#pragma once
#include "flock/web.hpp"
#include <httplib.h>

namespace flockv0::web {
bool bridgeEndpoint(std::string_view path);
bool artifactPreview(std::string_view path);
void channelRoutes(httplib::Server&, Store&, const WebCallbacks&);
}
