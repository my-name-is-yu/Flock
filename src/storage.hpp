#pragma once
#include "flock/channels.hpp"
#include <map>
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
std::string actionId();
Json runJson(const RunRef&);
RunRef runRef(const Json&);

struct PeerExchange {
    std::string id, kind, phase, lastMessage;
    Id sender = 0, recipient = 0;
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

struct Store::Impl {
    sqlite3* db = nullptr;
    const std::filesystem::path path;
    mutable std::recursive_mutex mutex;
    SecretStore secrets{path.parent_path()};
    std::map<ChannelKind,ChannelConnector> channels;
    std::map<std::string,std::shared_ptr<LocalMachine>,std::less<>> localMachines;
    std::vector<std::string> rootPeople;
    std::string artifactOrigin, webOrigin;
    std::mutex deliveryMutex;
    void migrateAccess();
    void migrateChannels();
    void migrateLocalMachines();
    std::optional<Json> messageReceipt(std::string_view bot, std::string_view clientId, std::string_view text, Id replyTo) const;
    void saveChannelMessage(Id, std::string_view bot, const VerifiedChannelMessage&, bool replay);
    bool channelMessageAuthorized(Id, std::string_view bot) const;
    Json channelContext(Id, std::string_view bot, std::vector<std::string>* images = nullptr) const;
    std::optional<ChannelResource> channelAttachment(Id source, Id attachment, std::string_view bot) const;
    Json channelTool(Id, std::string_view bot, const Decision&);
    void channelLimitation(Id source, std::string_view bot, std::string_view reason, bool supportsReplies);
    bool permitsLocalMachineOwner(std::string_view owner, std::string_view bot) const;
    Json localMachineTool(std::string_view bot, std::string_view scope, const Decision&);
    void queueChannelReply(Id source, Id reply, const Json&);
    void queueChannelTaskUpdate(Id task, Id entry, std::string_view kind, const Json&);
    bool channelDeliveryDue() const;
    void deliverChannels();
    Json channelToolStatus(Id source, std::string_view bot, Json, std::vector<std::string>* images = nullptr) const;
    bool permitsChannel(std::string_view bot, const ChannelOrigin&, std::string_view sender, std::string_view person,
                        std::string_view operation) const;
    SecretRedactor redactor(std::string_view bot) const;
    Json accessTool(std::string_view bot, const Json& routing, const Decision&);
    Json readiness(std::string_view bot, const Json& routing, const Json& args, T3&);
    Seconds environmentTimeout(std::string_view bot, std::string_view environment, const Json& config) const;
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
    std::string messageModelBudget(Id messageId);
    std::string taskModelBudget(const Json& task);
    std::string ensureModelBudget(std::string key, const Json& config);
    Id reserveModelCall(std::string_view budget, std::string_view purpose, Id messageId, Id taskId, Id eventId);
    void grantModelCall(std::string_view budget);
    Decision callModel(std::string_view bot, Model&, const ModelInput&, Id attemptId);
    std::vector<std::pair<std::string,Id>> pendingConversations(unsigned limit) const;
    bool runTurn(Id messageId, Model&, T3&, const Runtime::NotesLoader&);
    Json prepareSessionEffect(const conversation::Message&, int step, const Decision&);
    std::optional<conversation::Message> turnMessage(Id messageId) const;
    Json turnInput(const conversation::Message&, int step, std::vector<std::string>& images) const;
    Json applyTurnTool(const conversation::Message&, int step, const Decision&);
    void finishTurn(const conversation::Message&, std::string text, const Json& suggestions, std::string_view route);
    void pauseTurn(const conversation::Message&);
    Json ownTask(Id taskId, const conversation::Message&) const;
    Json resultReference(Id messageId, const conversation::Message&) const;
    Id startConversationTask(const Json& routing, std::string_view bot, std::string_view request,
                             std::string_view environment, Id messageId, const Json& source, bool resume);
    bool acceptInstruction(Id taskId, Id revision, Id messageId, int step, std::string_view instruction,
                           const Json& source, const Json& routing, const TaskChange* change);
    Id addRevision(Json&, std::string_view intent, std::string_view doneWhen, Id sourceMessageId = 0);
    bool conversationPending(std::string_view bot) const;
    void save(Json&);
    Id submit(const Intake&, std::string_view peerKind = {}, Id sourceMessageId = 0,
              const Json& source = nullptr);
    Id submit(const Intake&, Id sourceMessageId, const Json& source);
    Id submit(const Intake&, Id sourceMessageId, const Json& source, bool peerTask);
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
