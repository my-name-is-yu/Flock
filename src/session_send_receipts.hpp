#pragma once

#include "session_send_receipt.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <map>
#include <optional>
#include <string_view>

namespace flockv0 {
class Database;
class SecretStorage;
class SessionParticipationRecords;
class TaskRecords;
class TaskRunReports;

// What T3 answered when a turn sent a message to a Session, and the record of the Run that send started.
class SessionSendReceipts {
public:
    SessionSendReceipts(Database& database, SecretStorage& secretStorage, SessionParticipationRecords& sessionParticipations,
                        TaskRecords& taskRecords, TaskRunReports& taskRunReports)
        : database(database), secretStorage(secretStorage), sessionParticipations(sessionParticipations), taskRecords(taskRecords),
          taskRunReports(taskRunReports) {}

    bool persistPending(Id message);
    void forgetPending(Id message);
    void recordPendingRuns(const SessionTarget&);
    void persist(Id message, const SessionSendReceipt&, std::optional<Seconds> interruptAt = {});
    void saveResult(std::string_view bot, Id message, int step, Json result);
    // Receipts of sends whose turn has not finished, by message.
    std::map<Id, SessionSendReceipt> pending;
private:
    bool recordRun(Id message, const SessionSendReceipt&, std::optional<Seconds> interruptAt = {});

    Database& database;
    SecretStorage& secretStorage;
    SessionParticipationRecords& sessionParticipations;
    TaskRecords& taskRecords;
    TaskRunReports& taskRunReports;
};
}
