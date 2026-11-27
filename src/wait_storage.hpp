#pragma once

#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/wait.hpp"

#include <optional>
#include <string_view>
namespace flockv0 {
struct TaskData;
class RecordDecoder;
class Statement;

Json to_json(const Wait&);
Json encodeWaitData(const Wait&);
Wait decodeWait(Id id, std::string_view reason, std::string_view data);
Wait decodeWaitRow(const Statement&, int col = 0);
Wait decodeWaitingEntry(Id entry, const Json& data);
Json waitRunJson(const std::optional<WaitRun>&);
std::optional<WaitRun> decodeWaitRun(const Json&, const RecordDecoder&);
std::optional<WaitRun> taskWaitRun(const TaskData& task);

class Database;
class JournalStorage;

class WaitStorage {
public:
    WaitStorage(Database& database, JournalStorage& journalStorage) : database(database), journalStorage(journalStorage) {}

    bool waitIsActionable(const TaskData& task) const;
    Id waitAnchor(Id task, Id wait) const;
    Id wait(TaskData&, Wait);
    Id wait(TaskData&, WaitDetails, std::string text, std::optional<WaitRun> run = {});
    std::optional<Wait> currentWait(Id task) const;
    std::optional<Wait> projectedWait(Id task, const Statement&, int col) const;
    void clearWait(TaskData&, std::string_view resolution);
    void unlockQuestion(TaskData&, std::string_view reason);
private:
    Database& database;
    JournalStorage& journalStorage;
};
}
