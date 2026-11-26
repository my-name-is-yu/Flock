#pragma once

#include "flock/json.hpp"

namespace flockv0 {
struct RunRef;
Json runJson(const RunRef&);
RunRef runRef(const Json&);
}
