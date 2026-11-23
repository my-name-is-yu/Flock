#pragma once
#include "flock/web.hpp"
#include <httplib.h>

namespace flockv0::web {
Person person(const WebCallbacks&, const httplib::Request&);
void memoryAutomationRoutes(httplib::Server&, Store&, const WebCallbacks&);
void settingsRoutes(httplib::Server&, Store&, const WebCallbacks&);
}
