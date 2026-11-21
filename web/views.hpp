#pragma once
#include "flock/core.hpp"

namespace flockv0::web {
std::string field(const Json&, std::string_view);
Json botList(Store&, const Json&);
Json botSettings(const Json&);
Json botView(Store&, const Json&);
Json conversation(Store&, std::string_view bot, Id before);
Json peerConversation(Store&, std::string_view bot, std::string_view peer, Id before);
Json sessions(Store&, std::string_view bot, Id before);
Json taskDetail(const Json& task, const Json& latestResult);
} // namespace flockv0::web
