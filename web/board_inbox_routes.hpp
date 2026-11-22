#pragma once
#include "flock/web.hpp"
#include <httplib.h>

namespace flockv0::web {
void boardInboxRoutes(httplib::Server&, Store&, const WebCallbacks&);
}
