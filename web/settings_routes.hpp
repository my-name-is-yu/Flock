#pragma once
#include "flock/web.hpp"
#include <httplib.h>

namespace flockv0::web {
Person person(const WebCallbacks&, const httplib::Request&);
bool projectAvailable(Store&, const WebCallbacks&, const Person&, std::string_view environment, std::string_view project);
bool hasWebIdentities(const WebCallbacks&);
void memoryAutomationRoutes(httplib::Server&, Store&, const WebCallbacks&);
void settingsRoutes(httplib::Server&, Store&, const WebCallbacks&);
}
