#pragma once
#include "flock/web.hpp"
#include <httplib.h>

namespace flockv0::web {
void sessionHandoffRoutes(httplib::Server&, Store&, const WebCallbacks&);
}
