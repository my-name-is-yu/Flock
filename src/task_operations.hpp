#pragma once

#include <optional>
#include <string_view>
#include "flock/json.hpp"
#include "flock/outbox_instruction.hpp"
#include "flock/task_context.hpp"
#include "flock/types.hpp"
#include "session_stop.hpp"

namespace flockv0 {

class Database;
class Policy;
class BotLifecycleStorage;
class InboxStorage;
class JournalStorage;
class OutboxStorage;
class SecretStorage;
class TaskEvents;
class TaskRecords;
class TaskRouting;
class WaitStorage;
struct Person;
struct Intake;
struct TaskData;
struct TaskTarget;
struct TaskChange;
struct TaskRoute;
struct WorkSession;
enum class InboxKind;

class TaskOperations {
public:
    TaskOperations(Database& database, BotLifecycleStorage& botLifecycle, const Policy& permissions, JournalStorage& journalStorage,
                   OutboxStorage& outboxStorage, SecretStorage& secretStorage, TaskEvents& taskEvents, TaskRecords& taskRecords,
                   TaskRouting& taskRouting, WaitStorage& waitStorage, InboxStorage& inboxStorage)
        : database(database), botLifecycle(botLifecycle), permissions(permissions), journalStorage(journalStorage),
          outboxStorage(outboxStorage), secretStorage(secretStorage), taskEvents(taskEvents), taskRecords(taskRecords),
          taskRouting(taskRouting), waitStorage(waitStorage), inboxStorage(inboxStorage) {}

    void recordTaskBlocker(TaskData&, std::string_view reason, Id waitId);
    void withdrawTaskQuestion(TaskData&, Id waitId);
    Id startConversationTask(const Json& routing, std::string_view bot, std::string_view request, const TaskRoute&, Id messageId,
                             const std::optional<OutboxSource>& source, bool resume, const WorkSession* selectedSession = nullptr);
    bool acceptInstruction(TaskData& task, Id messageId, int step, std::string_view instruction, std::optional<OutboxSource> source,
                           const TaskChange* change);
    Id submitRequest(const Intake&, const Person* requester);
    Id submit(const Intake&, Id sourceMessageId = 0, const OutboxSource* source = nullptr, std::optional<TaskTrigger> trigger = {},
              std::optional<std::string> requester = {});
    void operate(const Person&, const TaskTarget&, std::string_view, const Json&, Id sourceMessage = 0, std::optional<InboxKind> = {});
    void requestCancellation(TaskData&, SessionStop stop = {});
private:
    Database& database;
    BotLifecycleStorage& botLifecycle;
    const Policy& permissions;
    JournalStorage& journalStorage;
    OutboxStorage& outboxStorage;
    SecretStorage& secretStorage;
    TaskEvents& taskEvents;
    TaskRecords& taskRecords;
    TaskRouting& taskRouting;
    WaitStorage& waitStorage;
    InboxStorage& inboxStorage;
};
}
