#pragma once
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
struct Person;
class Store;
struct TaskView;
}

namespace flockv0::web {
std::string field(const Json&, std::string_view);
std::string externalSessionLink(const Json& run);
Json botList(Store&, const Json&);
Json botSettings(const Json&);
void projectMemoryHistoryLinks(Json&);
Json botView(Store&, const Json&, const std::vector<TaskView>* changedSessionTasks = nullptr, const Person* viewer = nullptr);
Json conversation(Store&, std::string_view bot, Id before, const Person* viewer = nullptr);
Json sessions(Store&, std::string_view bot, Id before);
Json sessionGroups(const std::vector<TaskView>& tasks);
} // namespace flockv0::web
