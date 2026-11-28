#pragma once

#include <string_view>

namespace flockv0 {
class Database;

bool t3EnvironmentConnected(const Database&, std::string_view environment);
}
