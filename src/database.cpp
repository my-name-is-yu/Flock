#include "database.hpp"
#include "record_decode.hpp"
#include "flock/run_ref.hpp"
#include "flock/wait.hpp"

#include <array>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace flockv0 {
namespace {
constexpr std::chrono::milliseconds storageBusyTimeout{4000};

[[noreturn]] void sqlError(sqlite3* db) {
    throw StorageError(db);
}

constexpr std::array kinds{
    std::pair{JournalKind::waiting, std::string_view{"waiting"}},
    std::pair{JournalKind::user_answer, std::string_view{"user_answer"}},
    std::pair{JournalKind::send_started, std::string_view{"send_started"}},
    std::pair{JournalKind::report, std::string_view{"report"}},
    std::pair{JournalKind::run_settled, std::string_view{"run_settled"}},
    std::pair{JournalKind::interrupt_requested, std::string_view{"interrupt_requested"}},
    std::pair{JournalKind::instruction_accepted, std::string_view{"instruction_accepted"}},
    std::pair{JournalKind::instruction_delivered, std::string_view{"instruction_delivered"}},
    std::pair{JournalKind::task_revision, std::string_view{"task_revision"}},
    std::pair{JournalKind::bot_message, std::string_view{"bot_message"}},
    std::pair{JournalKind::read_bot_evidence, std::string_view{"read_bot_evidence"}},
    std::pair{JournalKind::pr_review_requested, std::string_view{"pr_review_requested"}},
    std::pair{JournalKind::git_pr_followup_requested, std::string_view{"git_pr_followup_requested"}},
    std::pair{JournalKind::conversation_message, std::string_view{"conversation_message"}},
    std::pair{JournalKind::conversation_reply, std::string_view{"conversation_reply"}},
    std::pair{JournalKind::conversation_budget_approved, std::string_view{"conversation_budget_approved"}},
    std::pair{JournalKind::completed, std::string_view{"completed"}},
    std::pair{JournalKind::cancel_requested, std::string_view{"cancel_requested"}},
    std::pair{JournalKind::cancelled, std::string_view{"cancelled"}},
    std::pair{JournalKind::tool_error, std::string_view{"tool_error"}},
    std::pair{JournalKind::decision, std::string_view{"decision"}},
};

template <class T> Json sqlValue(const std::optional<T>& value) {
    return value ? Json(*value) : Json(nullptr);
}

Json sqlFlag(std::optional<bool> value) {
    return value ? Json(*value ? 1 : 0) : Json(nullptr);
}

}

StorageError::StorageError(sqlite3* db) : std::runtime_error(sqlite3_errmsg(db)), code_(sqlite3_errcode(db) & 0xff) {}

Statement::Statement(sqlite3* db, const char* sql, const Json& values) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) sqlError(db);
    try {
        int i = 0;
        for (const auto& value : values) {
            ++i;
            int rc;
            if (value.is_null())
                rc = sqlite3_bind_null(stmt_, i);
            else if (value.is_number_integer())
                rc = sqlite3_bind_int64(stmt_, i, value.get<Id>());
            else {
                const auto encoded = value.is_string() ? value.get<std::string>() : value.dump();
                rc = sqlite3_bind_text(stmt_, i, encoded.data(), static_cast<int>(encoded.size()), SQLITE_TRANSIENT);
            }
            if (rc != SQLITE_OK) sqlError(db);
        }
    } catch (...) {
        sqlite3_finalize(stmt_);
        throw;
    }
}
bool Statement::next() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    sqlError(sqlite3_db_handle(stmt_));
}
std::string Statement::string(int col) const {
    const auto* data = sqlite3_column_text(stmt_, col);
    return data ? std::string(reinterpret_cast<const char*>(data), sqlite3_column_bytes(stmt_, col)) : std::string{};
}

bool StorageError::contended() const {
    return code_ == SQLITE_BUSY || code_ == SQLITE_LOCKED;
}

Statement::~Statement() {
    sqlite3_finalize(stmt_);
}

bool Statement::null(int col) const {
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

Id Statement::number(int col) const {
    return sqlite3_column_int64(stmt_, col);
}

Json Statement::json(int col) const {
    return Json::parse(string(col));
}

Transaction::Transaction(sqlite3* db, Mode mode) : db_(db) {
    Statement(db_, mode == Mode::write ? "BEGIN IMMEDIATE" : "BEGIN", Json::array()).next();
}

Transaction::~Transaction() {
    if (!done_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
}

void Transaction::commit() {
    Statement(db_, "COMMIT", Json::array()).next();
    done_ = true;
}

void Database::open(const std::filesystem::path& path) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) sqlError(db);
    sqlite3_busy_timeout(db, storageBusyTimeout.count());
    const auto deadline = std::chrono::steady_clock::now() + storageBusyTimeout;
    for (;;) {
        try {
            openSchema();
            return;
        } catch (const StorageError& error) {
            // SQLite can skip the busy handler while a competing connection switches to WAL.
            if (!error.contended() || std::chrono::steady_clock::now() >= deadline) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

Database::~Database() {
    sqlite3_close(db);
}

Statement Database::query(const char* sql) const {
    return query(sql, Json::array());
}

Statement Database::query(const char* sql, const Json& args) const {
    return Statement(db, sql, args);
}

void Database::exec(const char* sql) const {
    query(sql).next();
}

void Database::exec(const char* sql, const Json& args) const {
    query(sql, args).next();
}

Id Database::last() const {
    return sqlite3_last_insert_rowid(db);
}

Id Database::dataVersion() const {
    auto version = query("PRAGMA data_version");
    return version.next() ? version.number(0) : 0;
}

std::pair<Id, Id> Database::changeStamp() const {
    std::lock_guard lock(mutex);
    return {sqlite3_total_changes64(db), dataVersion()};
}

Id Database::writeJournal(std::optional<Id> task, JournalKind kind, const Json& data, const JournalKeys& keys,
                          std::optional<std::string_view> conversationBot) {
    const auto* run = keys.run;
    exec(R"SQL(
INSERT INTO journal(task_id,time,kind,data,conversation_bot_id,wait_id,wait_reason,user_answer_conversation,
                    action_id,run_environment_id,run_thread_id,run_id,run_gateway_id,result_run_id,run_status,revision_id,stop_requested,
                    message_id,instruction_step,exchange_id,peer_message_id,forwarded_source_message_id,pr_review_item_id,
                    git_event_id,operation_id,reply_to_message_id,task_question_message_id,wait_message_id,conversation_route,
                    conversation_status,session_participation_id,session_stop_id,feedback_event_id,workflow_outcome)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
)SQL",
         {sqlValue(task),
          unixTime(),
          enumName(kinds, kind),
          data,
          sqlValue(conversationBot),
          sqlValue(keys.waitId),
          keys.waitReason ? Json(waitReasonName(*keys.waitReason)) : Json(nullptr),
          sqlFlag(keys.answerInConversation),
          sqlValue(keys.actionId),
          sqlValue(keys.environmentId),
          run ? Json(run->threadId) : Json(nullptr),
          run ? Json(run->runId) : Json(nullptr),
          run ? (run->gatewayEnvironmentId.empty() ? Json(nullptr) : Json(run->gatewayEnvironmentId)) : Json(nullptr),
          sqlValue(keys.resultRunId),
          sqlValue(keys.status),
          sqlValue(keys.revisionId),
          sqlFlag(keys.stopRequested),
          sqlValue(keys.messageId),
          sqlValue(keys.instructionStep),
          sqlValue(keys.exchangeId),
          sqlValue(keys.peerMessageId),
          sqlValue(keys.forwardedSourceMessageId),
          sqlValue(keys.prReviewItemId),
          sqlValue(keys.gitEventId),
          sqlValue(keys.operationId),
          sqlValue(keys.replyToMessageId),
          sqlValue(keys.taskQuestionMessageId),
          sqlValue(keys.waitMessageId),
          sqlValue(keys.conversationRoute),
          sqlValue(keys.conversationStatus),
          sqlValue(keys.sessionParticipationId),
          sqlValue(keys.sessionStopId),
          sqlValue(keys.feedbackEventId),
          sqlValue(keys.workflowOutcome)});
    return last();
}

Id Database::writeJournal(Id id, std::string_view kind, const Json& data) {
    exec("INSERT INTO journal(task_id,time,kind,data) VALUES(?,?,?,?)", {id, unixTime(), kind, data});
    return last();
}

}
