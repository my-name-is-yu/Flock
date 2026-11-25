#pragma once

#include "runtime_components.hpp"
#include "storage.hpp"
#include "flock/types.hpp"
#include "flock/intake.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/t3_command.hpp"
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"
#include "flock/outbox.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace flockv0 {
class Runtime::Components::OutboxDispatch {
public:
    OutboxDispatch(Store::Impl& db, T3& t3, const Policy& policy, TaskUpdates& updates, TaskQuestions& questions, RunReports& reports,
                   RunObserver& runs, SessionPrompts& prompts, const std::function<Seconds()>& clock, const IntakeResolver& intake,
                   bool requireT3Connection, std::stop_token shutdown)
        : db(db), t3(t3), policy(policy), updates(updates), questions(questions), reports(reports), runs(runs), prompts(prompts),
          clock(clock), intake(intake), requireT3Connection(requireT3Connection), shutdown(shutdown) {}

    void stop(TaskData task);
    void step(const TaskData& task);
    void stepDestination(const TaskData& task, const DestinationRun& run);
    void queueAction(TaskData& task, const std::string& id, CommandTool tool, std::string instruction, const Json& args, const Json& event,
                     bool charge, const TaskRoute* destination = nullptr);
    void queueDestinations(TaskData& task, const std::string& first, std::string_view instruction, const Json& args, const Json& event,
                           bool charge);
private:
    Command command(const TaskData& task, const Outbox& action);
    void deliver(const TaskData& task, Outbox action);
    void applyDelivery(Id id, const Outbox& action, Delivery outcome);
    void writeDestinationReport(TaskData& task, DestinationRun& run, const Observation& observed);
    static bool setDestinationStatus(DestinationRun& run, DestinationRunPhase status, std::string detail);
    void destinationStatus(Id id, std::string_view action, DestinationRunPhase status, std::string detail);
    bool destinationDeadline(TaskData& task, DestinationRun& run, Seconds now);
    enum class DeliveryOutcome { Accepted, DefinitelyUnsent, Unknown };

    void instructionDelivery(TaskData& task, const Outbox& action);
    bool assignedRunMatches(const TaskData& task, const CommandPayload& command, const RunRef& run);
    DeliveryOutcome classifyDelivery(const TaskData& task, const Outbox& action, Delivery& outcome, bool reply);
    AssignedRun recordRunAssignment(const TaskData& task, const Outbox& action, const RunRef& ref, DestinationRun* destination = nullptr);
    void rememberRunCandidate(Outbox action, const RunRef& ref);
    bool deferDestinationDelivery(TaskData& task, DestinationRun& run, const Outbox& action, const Delivery& outcome,
                                  DeliveryOutcome delivery);

    Store::Impl& db;
    T3& t3;
    const Policy& policy;
    TaskUpdates& updates;
    TaskQuestions& questions;
    RunReports& reports;
    RunObserver& runs;
    SessionPrompts& prompts;
    const std::function<Seconds()>& clock;
    const IntakeResolver& intake;
    bool requireT3Connection;
    std::stop_token shutdown;
};
}
