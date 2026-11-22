#pragma once
#include "flock/web.hpp"
#include <httplib.h>

namespace flockv0::web {
Person person(const WebCallbacks&, const httplib::Request&);
void accessRoutes(httplib::Server&, Store&, const WebCallbacks&);
}
