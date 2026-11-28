#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

struct sqlite3;
struct sqlite3_stmt;

namespace flockv0 {
struct RunRef;
class Store;
enum class WaitReason;

enum class JournalKind {
    waiting,
    user_answer,
    send_started,
    report,
    run_settled,
    interrupt_requested,
    instruction_accepted,
    instruction_delivered,
    task_revision,
    git_pr_followup_requested,
    conversation_message,
    conversation_reply,
    memory_learned,
    completed,
    cancel_requested,
    cancelled,
    tool_error,
    decision
};

struct JournalKeys {
    std::optional<Id> waitId, revisionId;
    std::optional<WaitReason> waitReason;
    std::optional<bool> answerInConversation, stopRequested;
    const RunRef* run = nullptr;
    std::optional<std::string_view> environmentId, resultRunId, status, actionId;
    std::optional<Id> messageId;
    std::optional<int> instructionStep;
    std::optional<std::string_view> gitEventId, operationId;
    std::optional<Id> replyToMessageId, taskQuestionMessageId;
    std::optional<std::string_view> conversationRoute, conversationStatus;
    std::optional<Id> sessionParticipationId, sessionStopId, feedbackEventId;
};

class StorageError : public std::runtime_error {
public:
    explicit StorageError(sqlite3* db);
    bool contended() const;
private:
    int code_;
};

class Statement {
public:
    Statement(sqlite3*, const char*, const Json& values);
    ~Statement();
    Statement(const Statement&) = delete;
    bool next();
    bool null(int col) const;
    Id number(int col) const;
    std::string string(int col) const;
    Json json(int col) const;
private:
    sqlite3_stmt* stmt_ = nullptr;
};

class Transaction {
public:
    enum class Mode { write, read };
    explicit Transaction(sqlite3* db, Mode mode = Mode::write);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();
private:
    sqlite3* db_;
    bool done_ = false;
};

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    static Database& of(Store&);
    void open(const std::filesystem::path&);
    Statement query(const char*) const;
    Statement query(const char*, const Json&) const;
    void exec(const char*) const;
    void exec(const char*, const Json&) const;
    Id dataVersion() const;
    std::pair<Id, Id> changeStamp() const;
    Id last() const;
    Id writeJournal(Id task, std::string_view kind, const Json&);
    struct BotJournalEntry {
        std::string_view bot, kind;
        Seconds time;
        std::optional<Id> task = {};
        std::optional<std::string_view> operationId = {};
    };
    // An entry in a Bot's conversation that does not need a task.
    Id writeJournal(const BotJournalEntry&, const Json&);
    Id writeJournal(std::optional<Id>, JournalKind, const Json&, const JournalKeys&, std::optional<std::string_view> conversationBot = {});

    sqlite3* db = nullptr;
    mutable std::recursive_mutex mutex;
private:
    void openSchema();
};
}
