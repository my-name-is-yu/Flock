#include "database.hpp"
#include "wait_storage.hpp"
#include "task_routing.hpp"
#include "flock/types.hpp"
#include "flock/task.hpp"
#include "flock/wait.hpp"

#include <string_view>

namespace flockv0 {
bool TaskRouting::automationBeforeDispatch(Id task) const {
    return database.query("SELECT 1 FROM automation_before_dispatch WHERE id=?", {task}).next();
}

bool TaskRouting::isUndispatchedNotesWait(const TaskData& task) const {
    if (task.state != TaskState::waiting || !automationBeforeDispatch(task.id)) return false;
    const auto wait = waitStorage.currentWait(task.id);
    return wait && wait->reason() == WaitReason::bot_notes_unavailable;
}

bool TaskRouting::projectRouteReady(Id task, std::string_view action) const {
    return database.query("SELECT 1 FROM project_route_slots WHERE id=? AND action_id=? AND blocker_id IS NULL", {task, action}).next();
}
}
