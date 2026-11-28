#pragma once

#include "runtime_components.hpp"
#include "flock/intake.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"
#include "flock/outbox.hpp"

#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <filesystem>

namespace flockv0 {
struct Command;
struct Delivery;
struct Observation;
struct Wait;

class BotLifecycleStorage;
class ConversationMessages;
class Database;
class JournalStorage;
class Policy;
class PolicyApprovals;
class SessionParticipationRecords;
class SessionReadStorage;
class T3;
class TaskEvents;
class TaskRecords;
class TaskRouting;
class TaskRunReports;
class WaitStorage;

class OutboxStorage;

class runtime::OutboxDispatch {
public:
    OutboxDispatch(Database& database, BotLifecycleStorage& botLifecycle, ConversationMessages& conversationMessages,
                   JournalStorage& journalStorage, const std::filesystem::path& path, PolicyApprovals& policyApprovals,
                   SessionParticipationRecords& sessionParticipations, SessionReadStorage& sessionReadStorage, TaskEvents& taskEvents,
                   TaskRecords& taskRecords, TaskRouting& taskRouting, TaskRunReports& taskRunReports, WaitStorage& waitStorage,
                   OutboxStorage& outboxStorage, T3& t3, const Policy& policy, TaskUpdates& updates, TaskQuestions& questions,
                   RunReports& reports, RunObserver& runs, SessionPrompts& prompts, const std::function<Seconds()>& clock,
                   bool requireT3Connection, const IntakeResolver& intake, std::stop_token shutdown)
        : database(database), botLifecycle(botLifecycle), conversationMessages(conversationMessages), journalStorage(journalStorage),
          path(path), policyApprovals(policyApprovals), sessionParticipations(sessionParticipations),
          sessionReadStorage(sessionReadStorage), taskEvents(taskEvents), taskRecords(taskRecords), taskRouting(taskRouting),
          taskRunReports(taskRunReports), waitStorage(waitStorage), outboxStorage(outboxStorage), t3(t3), policy(policy), updates(updates),
          questions(questions), reports(reports), runs(runs), prompts(prompts), clock(clock), requireT3Connection(requireT3Connection),
          intake(intake), shutdown(shutdown) {}

    void stop(TaskData task);
    bool step(TaskData& task, const Wait* wait);
    void stepDestination(const TaskData& task, const DestinationRun& run);
    bool queueAction(TaskData& task, const std::string& id, CommandTool tool, std::string instruction, const Json& args, const Json& event,
                     const TaskRoute* destination = nullptr);
    void queueDestinations(TaskData& task, const std::string& first, std::string_view instruction, const Json& args, const Json& event);
private:
    Command command(const TaskData& task, const Outbox& action);
    void deliver(const TaskData& task, Outbox action);
    void applyDelivery(Id id, const Outbox& action, Delivery outcome, std::optional<Id> readyGeneration = {});
    void writeDestinationReport(TaskData& task, DestinationRun& run, const Observation& observed);
    static bool setDestinationStatus(DestinationRun& run, DestinationRunPhase status, std::string detail);
    void destinationStatus(Id id, std::string_view action, DestinationRunPhase status, std::string detail);
    enum class DeliveryOutcome { Accepted, DefinitelyUnsent, Unknown };

    void instructionDelivery(TaskData& task, const Outbox& action);
    bool assignedRunMatches(const TaskData& task, const CommandPayload& command, const RunRef& run);
    DeliveryOutcome classifyDelivery(const TaskData& task, const Outbox& action, Delivery& outcome, bool reply);
    AssignedRun recordRunAssignment(const TaskData& task, const Outbox& action, const RunRef& ref, DestinationRun* destination = nullptr);
    bool refreshSessionSettings(TaskData&, const Outbox&);
    bool deferSingleDelivery(TaskData&, const Outbox&, OutboxPhase attemptedPhase, const Delivery&, DeliveryOutcome);
    bool deferDestinationDelivery(TaskData& task, DestinationRun& run, const Outbox& action, const Delivery& outcome,
                                  DeliveryOutcome delivery);

    Database& database;
    BotLifecycleStorage& botLifecycle;
    ConversationMessages& conversationMessages;
    JournalStorage& journalStorage;
    const std::filesystem::path& path;
    PolicyApprovals& policyApprovals;
    SessionParticipationRecords& sessionParticipations;
    SessionReadStorage& sessionReadStorage;
    TaskEvents& taskEvents;
    TaskRecords& taskRecords;
    TaskRouting& taskRouting;
    TaskRunReports& taskRunReports;
    WaitStorage& waitStorage;
    OutboxStorage& outboxStorage;
    T3& t3;
    const Policy& policy;
    TaskUpdates& updates;
    TaskQuestions& questions;
    RunReports& reports;
    RunObserver& runs;
    SessionPrompts& prompts;
    const std::function<Seconds()>& clock;
    bool requireT3Connection;
    const IntakeResolver& intake;
    std::stop_token shutdown;
};
}
