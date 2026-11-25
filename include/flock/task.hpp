#pragma once

#include "flock/outbox_instruction.hpp"
#include "flock/run_report.hpp"
#include "flock/session_ref.hpp"
#include "flock/wait.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
enum class TaskState { queued, deciding, sending, running, waiting, peer_waiting, cancelling, completed, cancelled };

std::string_view taskStateName(TaskState);

struct TaskRoute {
    std::string environmentId, projectId, gatewayEnvironmentId;
};

struct PendingDestination {
    TaskRoute route;
    bool pinned;
};

enum class RunStopIntent { cancel, replan };

struct RunStop {
    RunStopIntent intent;
    std::optional<AssignedRun> run;
    std::optional<std::int64_t> retryAt{}, reconcileAt{};
};

struct PendingRunReport {
    SettledRunReport report;
    std::int64_t retryAt;
    int attempts;
};

struct TaskData {
    std::int64_t id = 0, generation = 0;
    TaskState state = TaskState::queued;
    std::string botId, request, intent, environmentId, projectId, doneWhen;
    std::optional<std::string> gatewayEnvironmentId{};
    std::optional<PendingDestination> pendingDestination{};
    std::optional<std::int64_t> currentRevision{};
    std::string actionId{};
    std::int64_t eventId = 0, deadline = 0;
    std::optional<AssignedRun> activeRun{}, lastRun{};
    std::optional<RunStop> runStop{};
    bool replanning = false, queuedInT3 = false, cancelledByBotPause = false;
    std::optional<std::string> replanningDetail{}, cancellationDetail{};
    std::vector<OutboxInstruction> cancelledInstructions{};
    std::optional<PendingRunReport> pendingRunReport{};
    std::optional<std::int64_t> pendingStageReport{}, sessionReconcileAt{}, workflowReconcileAt{};
    std::optional<WorkSession> selectedSession{};
    std::optional<SessionOrigin> sessionOrigin{};
    bool newSessionRequired = false, freshContextRequested = false;
    std::optional<std::string> launchWarning{};
    nlohmann::json config = nlohmann::json::object();
    nlohmann::json rest = nlohmann::json::object();
};

bool terminal(const TaskData&);

struct TaskRevision {
    std::int64_t id, number;
    std::string intent, doneWhen;
    std::optional<std::int64_t> sourceMessageId;
};

struct TaskBlocker {
    std::int64_t id;
    std::string botId, request;
    TaskState state;
};

struct TaskPresence {
    TaskState state;
    bool replanning;
};

struct TaskView {
    TaskData task;
    std::optional<Wait> wait{};
    std::optional<TaskBlocker> blockedBy{};
    nlohmann::json pendingAction = nullptr, runs = nlohmann::json::array();
    std::vector<TaskRevision> revisions{};
};

nlohmann::json taskJson(const TaskView&);
nlohmann::json taskJson(const std::vector<TaskView>&);
}
