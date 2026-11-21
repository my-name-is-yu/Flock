#pragma once
#include "flock/core.hpp"
#include <mutex>
#include <sqlite3.h>

namespace flockv0 {
namespace conversation { struct Context; }
class Statement {
public:
    Statement(sqlite3*, const char*, const Json& values);
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    bool next();
    Id number(int col) const { return sqlite3_column_int64(stmt_, col); }
    std::string string(int col) const;
    Json json(int col) const { return Json::parse(string(col)); }
private:
    sqlite3_stmt* stmt_ = nullptr;
};
std::string field(const Json&, std::string_view);
bool terminal(const Json&);
std::string actionId();
Json runJson(const RunRef&);
RunRef runRef(const Json&);

struct Store::Impl {
    sqlite3* db = nullptr;
    const std::filesystem::path path;
    mutable std::recursive_mutex mutex;
    explicit Impl(std::filesystem::path path);
    ~Impl();
    Statement query(const char* sql, const Json& args = Json::array()) const { return Statement(db, sql, args); }
    void exec(const char*, const Json& args = Json::array()) const;
    Id last() const { return sqlite3_last_insert_rowid(db); }
    Json load(Id) const;
    void save(Json&);
    Id submit(const Intake&, Id sourceMessageId = 0, const Json& source = nullptr);
    void operate(Id, Id, std::string_view, const Json&, bool fromConversation = false);
    Id journal(Id, std::string_view kind, const Json&);
    void migrateConversation();
    void routePending();
    void routeMessage(Id);
    conversation::Context routeContext(std::string_view bot, Id message, std::string_view text,
                                       Id replyTo, const Json& routing);
    Id addRevision(Json&, std::string_view intent, std::string_view doneWhen, Id sourceMessageId = 0);
    bool completionHeld(const Json&) const;
    Json queueBlocker(const Json&, std::string_view environment) const;
    Id event(Json&, std::string_view key, Json data);
    Json currentEvent(const Json&) const;
    void wait(Json&, std::string_view reason, Json data = Json::object());
    void clearWait(Json&, std::string_view resolution);
    void consumeEvent(const Json&);
    Json outbox(const Json&) const;
    void unlockQuestion(Json&, std::string_view reason);
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { Statement(db_, "BEGIN IMMEDIATE", Json::array()).next(); }
    ~Transaction() { if (!done_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { Statement(db_, "COMMIT", Json::array()).next(); done_ = true; }
private:
    sqlite3* db_;
    bool done_ = false;
};
} // namespace flockv0
