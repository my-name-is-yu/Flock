#pragma once

#include "flock/json.hpp"
#include "flock/person.hpp"
#include <filesystem>

namespace flockv0 {
class Store;

namespace app {
class Bots;

void exportBotSkills(const std::filesystem::path& root, const Json& bots);
void syncT3BotSkills(const std::filesystem::path& home, Store&, const Bots&, const WebIdentityResolver&) noexcept;
}
}
