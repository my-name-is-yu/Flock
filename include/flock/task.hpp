#pragma once

#include "flock/outbox.hpp"
#include "flock/outbox_instruction.hpp"
#include "flock/run_report.hpp"
#include "flock/session_ref.hpp"
#include "flock/task_context.hpp"
#include "flock/task_interaction.hpp"
#include "flock/wait.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace flockv0 {
enum class TaskState { queued, deciding, sending, running, waiting, external_waiting, cancelling, completed, cancelled };

std::string_view taskStateName(TaskState);

struct TaskRoute {
    std::string environmentId, projectId, gatewayEnvironmentId;
};

struct RunTarget {
    std::string name;
    TaskRoute route;
};

enum class DestinationRunPhase { queued, running, confirmation_pending };
using DestinationRunStatus = std::variant<DestinationRunPhase, std::string>;

struct DestinationReport {
    std::string commit, text, sessionCreatedAt;
    RunEvidence::Availability evidence;
};

struct DestinationRun {
    std::string actionId, name, environmentId;
    RunAssignment assignment{};
    RunRef ref{};
    bool explicitDirectGateway = false;
    DestinationRunStatus status = DestinationRunPhase::queued;
    std::optional<std::int64_t> retryAt{}, stopRetryAt{};
    unsigned evidenceAttempts = 0;
    std::optional<DestinationReport> report{};
    std::optional<std::string> detail{};

    bool outstanding() const { return std::holds_alternative<DestinationRunPhase>(status); }
    bool inPhase(DestinationRunPhase phase) const { return status == DestinationRunStatus{phase}; }
};

std::string_view destinationRunStatusName(const DestinationRunStatus&);
enum class RunOutcome { all_completed, partial_failure, failed };
std::string_view runOutcomeName(RunOutcome);

using RunTargets = std::vector<RunTarget>;

struct RunResults {
    std::vector<DestinationRun> runs;
    std::optional<RunOutcome> outcome{};
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

struct TaskCriteriaProposal {
    std::string doneWhen;
    std::optional<std::int64_t> sourceMessageId;
};

struct TaskData {
    std::int64_t id = 0, generation = 0;
    TaskState state = TaskState::queued;
    std::string botId, request, intent, environmentId, projectId, doneWhen;
    std::optional<std::string> gatewayEnvironmentId{};
    std::optional<PendingDestination> pendingDestination{};
    std::optional<std::int64_t> currentRevision{};
    std::string actionId{};
    std::int64_t eventId = 0;
    std::optional<AssignedRun> activeRun{}, lastRun{};
    std::optional<RunStop> runStop{};
    std::variant<std::monostate, RunTargets, RunResults> destinations{};
    bool replanning = false, queuedInT3 = false, cancelledByBotPause = false;
    std::optional<std::string> replanningDetail{}, cancellationDetail{};
    std::vector<OutboxInstruction> pendingInstructions{}, cancelledInstructions{};
    std::vector<TaskAttachmentRef> initialAttachments{};
    std::optional<PendingRunReport> pendingRunReport{};
    std::optional<std::int64_t> t3CheckAt{};
    std::optional<WorkSession> selectedSession{};
    std::optional<SessionOrigin> sessionOrigin{};
    bool newSessionRequired = false, freshContextRequested = false;
    std::optional<std::string> launchWarning{};
    std::int64_t createdAt = 0;
    std::optional<TaskCriteriaProposal> criteriaProposal{};
    std::optional<std::string> requestedCommit{};
    std::optional<std::string> waitingNote{};
    std::optional<std::int64_t> externalCheckAt{};
    std::optional<TaskQuestion> question{};
    std::optional<RecordedTaskBlocker> blocker{};
    std::optional<std::int64_t> sourceMessageId{}, sourceTaskId{};
    std::optional<bool> sourceMessageCoversFullRequest{};
    std::optional<std::string> requester{};
    std::vector<OutboxSource> sourceResultRefs{};
    std::optional<TaskRestart> restartContext{};
    std::optional<TaskTrigger> trigger{};
    nlohmann::json config = nlohmann::json::object();

    bool hasDestinations() const { return !std::holds_alternative<std::monostate>(destinations); }
    std::span<const RunTarget> runTargets() const {
        if (const auto* targets = std::get_if<RunTargets>(&destinations)) return *targets;
        return {};
    }
    std::span<const DestinationRun> runResults() const {
        if (const auto* results = std::get_if<RunResults>(&destinations)) return results->runs;
        return {};
    }
    std::optional<RunOutcome> runOutcome() const {
        if (const auto* results = std::get_if<RunResults>(&destinations)) return results->outcome;
        return std::nullopt;
    }
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

struct TaskRunView {
    AssignedRun run;
    std::string actionId;
    std::optional<std::string> status{}, sessionCreatedAt{};
    bool operator==(const TaskRunView&) const = default;
};

struct TaskView {
    TaskData task;
    std::optional<Wait> wait{};
    std::optional<TaskBlocker> blockedBy{};
    std::optional<OutboxAction> pendingAction{};
    std::vector<TaskRunView> runs{};
    std::vector<TaskRevision> revisions{};
};

nlohmann::json taskJson(const TaskView&);
nlohmann::json taskJson(const std::vector<TaskView>&);
}
