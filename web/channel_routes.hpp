#pragma once
#include "flock/web.hpp"
#include <httplib.h>

namespace flockv0::web {
bool channelWebhook(std::string_view path);
bool artifactPreview(std::string_view path);
void channelRoutes(httplib::Server&, Store&, const WebCallbacks&);
}
