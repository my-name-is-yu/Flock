#pragma once
#include "flock/core.hpp"
#include <mutex>
#include <sqlite3.h>
#include <utility>

namespace flockv0 {
namespace conversation { struct Context; struct Message; struct Plan; }
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

enum class RequestJournal { record, skip };

struct PeerExchange {
    std::string id, kind, phase, lastMessage;
    Id sender = 0, recipient = 0;
};

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
    Json taskDetails(Id, bool includeBlocker) const;
    Json queueBlocker(Id) const;
    void migrateConversation();
    void migrateModelBudget();
    std::string messageModelBudget(Id messageId, Id clarificationReplyId = 0);
    void inheritModelBudget(Id messageId, Id clarificationReplyId);
    std::string taskModelBudget(const Json& task);
    std::string ensureModelBudget(std::string key, const Json& config);
    Id reserveModelCall(std::string_view budget, std::string_view purpose, Id messageId, Id taskId, Id eventId);
    void grantModelCall(std::string_view budget);
    std::optional<Decision> savedRouteCall(Id messageId, unsigned step, const Json& input) const;
    Decision callModel(Model&, const ModelInput&, Id attemptId, unsigned routeStep = 0,
                       const Json& routeInput = nullptr);
    std::vector<std::pair<std::string,Id>> pendingConversations(unsigned limit) const;
    bool routeMessage(Id, Model&, const Runtime::NotesLoader&);
    void applyRoutes(const conversation::Message&, const conversation::Context&, const conversation::Plan&);
    conversation::Context routeContext(std::string_view bot, Id message, std::string_view text,
                                       Id replyTo, const Json& routing);
    Json captureConversationWaits(std::string_view bot) const;
    Id addRevision(Json&, std::string_view intent, std::string_view doneWhen, Id sourceMessageId = 0);
    bool conversationPending(std::string_view bot) const;
    bool completionHeld(const Json&) const;
    void save(Json&);
    Id submit(const Intake&, Id sourceMessageId = 0, const Json& source = nullptr,
              RequestJournal requestJournal = RequestJournal::record);
    void operate(Id, Id, std::string_view, const Json&, bool fromConversation = false);
    std::optional<PeerExchange> peerExchange(Id) const;
    void peerMessage(const PeerExchange&, const Json&);
    void requestCancellation(Json&);
    Id journal(Id, std::string_view kind, const Json&);
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
    enum class Mode { write, read };
    explicit Transaction(sqlite3* db, Mode mode = Mode::write) : db_(db) {
        Statement(db_, mode == Mode::write ? "BEGIN IMMEDIATE" : "BEGIN", Json::array()).next();
    }
    ~Transaction() { if (!done_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { Statement(db_, "COMMIT", Json::array()).next(); done_ = true; }
private:
    sqlite3* db_;
    bool done_ = false;
};
}
