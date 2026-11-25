#include "storage.hpp"

namespace flockv0 {
bool Store::Impl::automationBeforeDispatch(Id task) const {
    return query("SELECT 1 FROM automation_before_dispatch WHERE id=?", {task}).next();
}

bool Store::Impl::isUndispatchedNotesWait(const Json& task) const {
    return task.at("state") == "waiting" && field(task.at("wait"), "reason") == "bot_notes_unavailable" &&
           automationBeforeDispatch(task.at("id"));
}

bool Store::Impl::projectRouteReady(Id task, std::string_view action) const {
    return query("SELECT 1 FROM project_route_slots WHERE id=? AND action_id=? AND blocker_id IS NULL", {task, action}).next();
}
}
