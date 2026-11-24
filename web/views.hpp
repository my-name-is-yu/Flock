#pragma once
#include "flock/core.hpp"

namespace flockv0::web {
std::string field(const Json&, std::string_view);
Json botList(Store&, const Json&);
Json botSettings(const Json&);
Json botView(Store&, const Json&, const Json* changedSessionTasks = nullptr, std::string_view viewer = {});
Json conversation(Store&, std::string_view bot, Id before, std::string_view viewer = {});
Json peerConversation(Store&, std::string_view bot, std::string_view peer, Id before, Id anchor = 0,
                      std::string_view viewer = {});
Json sessions(Store&, std::string_view bot, Id before);
Json sessionGroups(const Json& tasks);
} // namespace flockv0::web
