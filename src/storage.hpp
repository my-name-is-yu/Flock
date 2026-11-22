#pragma once
#include "flock/access.hpp"
#include "flock/memory_automation.hpp"
#include <mutex>
#include <sqlite3.h>
#include <utility>

namespace flockv0 {
namespace conversation { struct Message; }
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
Json questionTarget(const Json&);
std::string actionId();
std::string taskIntent(const Json& task);
Json runJson(const RunRef&);
RunRef runRef(const Json&);

enum class RequestJournal { record, skip };

struct PeerExchange {
    std::string id, kind, phase, lastMessage;
    Id sender = 0, recipient = 0;
    bool allowsReplan(Id task) const { return recipient == task && kind == "work" && phase == "working"; }
};

struct TaskChange { std::string intent, doneWhen, environment; };

class SecretStore {
public:
    explicit SecretStore(const std::filesystem::path& stateDirectory);
    ~SecretStore();
    SecretStore(const SecretStore&) = delete;
    void insert(const SecretRef&, std::string_view value);
    std::string read(const SecretRef&) const;
private:
    int stateDirectory_;
};

class SecretRedactor {
public:
    explicit SecretRedactor(std::vector<std::string> values);
    std::string text(const std::string&) const;
    Json payload(const Json&) const;
    enum class Fields { protocol, external };
    Json record(Json, Fields = Fields::protocol) const;
    void requirePublic(const Json&) const;
private:
    std::vector<std::string> values_;
};

Id requiredInteger(const Json&, std::string_view key, Id minimum = 1);

struct Store::Impl {
    sqlite3* db = nullptr;
    const std::filesystem::path path;
    const MemorySeed memorySeed;
    mutable std::recursive_mutex mutex;
    SecretStore secrets{path.parent_path()};
    void initializeAccessSchema();
    void initializeMemorySchema();
    void initializeAutomationSchema();
    BotNotes loadNotes(std::string_view bot, const Runtime::SoulLoader&, Seconds now);
    void createMemory(std::string_view key, std::string_view bot, std::string_view id,
                      std::string_view content, Seconds now);
    std::string seedPrivateMemory(std::string_view key, std::string_view bot, const SecretRedactor&, Seconds now);
    Json memory(std::string_view bot, const Person*, std::string_view operation, const Json&,
                Seconds now);
    Json automationTool(std::string_view bot, const Json& routing, const Decision&, Seconds now);
    Json createAutomation(const Intake&, const Json&, std::string_view author, Seconds now);
    void syncSchedule(const ConfiguredSchedule&, Seconds now, const SecretRedactor&);
    Json automations(std::string_view bot, Id before) const;
    Json cancelAutomation(std::string_view bot, Id id);
    void fireDueAutomations(Seconds now);
    void fireAutomation(Id, const Statement&, Seconds now);
    SecretRedactor redactor(std::string_view bot) const;
    SecretRedactor sharedRedactor() const;
    Json accessTool(std::string_view bot, const Json& routing, const Decision&);
    Json readiness(std::string_view bot, const Json& routing, const Json& args, T3&);
    Seconds environmentTimeout(std::string_view bot, std::string_view environment, const Json& config) const;
    explicit Impl(std::filesystem::path path, MemorySeed);
    ~Impl();
    Statement query(const char* sql, const Json& args = Json::array()) const { return Statement(db, sql, args); }
    void exec(const char*, const Json& args = Json::array()) const;
    Id last() const { return sqlite3_last_insert_rowid(db); }
    Json load(Id) const;
    Json taskDetails(Id, bool includeBlocker) const;
    void addTaskDetails(Json&) const;
    Json queueBlocker(Id) const;
    void migrateConversation();
    void migrateModelBudget();
    std::string messageModelBudget(Id messageId);
    std::string taskModelBudget(const Json& task);
    std::string ensureModelBudget(std::string key, const Json& config);
    Id reserveModelCall(std::string_view budget, std::string_view purpose, Id messageId, Id taskId, Id eventId);
    void grantModelCall(std::string_view budget);
    Decision callModel(std::string_view bot, Model&, const ModelInput&, Id attemptId);
    std::vector<std::pair<std::string,Id>> pendingConversations(unsigned limit) const;
    bool runTurn(Id messageId, Model&, T3&, const Runtime::SoulLoader&, const std::function<Seconds()>&);
    Json prepareSessionEffect(const conversation::Message&, int step, const Decision&);
    bool turnPending(Id messageId) const;
    conversation::Message loadTurnMessage(Id messageId) const;
    const char* questionAnswerError(const Json& task, const conversation::Message&) const;
    Json taskState(const Json& task, const conversation::Message&) const;
    Json turnInput(const conversation::Message&, int step, Seconds now) const;
    Json applyTurnTool(const conversation::Message&, int step, const Decision&, Seconds now);
    void finishTurn(const conversation::Message&, std::string text, const Json& suggestions, std::string_view route);
    void pauseTurn(const conversation::Message&);
    Json ownTask(Id taskId, std::string_view bot) const;
    Json resultReference(Id messageId, std::string_view bot) const;
    Id startConversationTask(const Json& routing, std::string_view bot, std::string_view request,
                             std::string_view environment, Id messageId, const Json& source, bool resume);
    bool acceptInstruction(Id taskId, Id revision, Id messageId, int step, std::string_view instruction,
                           const Json& source, const Json& routing, const TaskChange* change);
    Id addRevision(Json&, std::string_view intent, std::string_view doneWhen, Id sourceMessageId = 0);
    bool conversationPending(std::string_view bot) const;
    void save(Json&);
    Id submit(const Intake&, Id sourceMessageId = 0, const Json& source = nullptr,
              RequestJournal requestJournal = RequestJournal::record, const Json& trigger = nullptr);
    void operate(Id, Id, std::string_view, const Json&, bool fromConversation = false);
    std::optional<PeerExchange> peerExchange(Id) const;
    bool replanReady(const Json& task) const;
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
