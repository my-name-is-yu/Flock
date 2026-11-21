#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
using Json = nlohmann::json;
using Id = std::int64_t;
using Seconds = std::int64_t;

Seconds unixTime();
std::string bounded(std::string_view text, std::size_t bytes);

struct Conflict : std::runtime_error { using std::runtime_error::runtime_error; };

// Routing and execution settings are saved at intake. Current instructions are loaded for each turn.
struct Intake {
    std::string botId, request, environmentId, projectId;
    Json config = Json::object();
    std::string scheduleId;
    Seconds scheduledAt = 0;
};
struct BotNotes { std::string soul, memory; };
struct PeerBot { std::string name; Intake intake; };

struct Decision {
    std::string tool;
    Json args = Json::object();
    std::string reason;
    Json usage = Json::object(); // Optional numeric measurements from the adapter.
};
struct ModelInput {
    Json task, event, history;
    BotNotes notes;
    std::vector<std::string> tools;
    std::string feedback;
    Json evidence = Json::array();
    Json config = Json::object(); // Adapter settings only. Never serialize into the prompt.

};
struct ModelError : std::runtime_error {
    ModelError(std::string message, bool retryable) : std::runtime_error(std::move(message)), transient(retryable) {}
    bool transient;
};
class Model {
public:
    virtual ~Model() = default;
    // Return exactly one tool call. Throw ModelError for transport/format failures.
    virtual Decision decide(const ModelInput&) = 0;
};

struct RunRef {
    std::string threadId, runId, link;
};
struct Question {
    std::string requestId;
    Json fields = Json::array();
};
struct Observation {
    enum class Kind { Running, Question, ExternalInput, Finished, Unavailable } kind = Kind::Running;
    Question question;
    std::string status, report, resultRunId, itemId;
    bool truncated = false;
    bool settled = false; // True on Finished only after delegated work has settled.
    std::string detail;
};
struct Command {
    std::string actionId, tool, environmentId, projectId, instruction, authGeneration;
    Json config = Json::object(), answers = Json::object();
    RunRef target;
    std::string questionId;
    RunRef candidate; // User-specified reconciliation hint. T3 must verify ownership.
    std::string candidateQuestionId;
};
struct Delivery {
    enum class Kind { Accepted, Unknown, Auth, Settings, QuestionGone, NotAccepted } kind = Kind::Unknown;
    RunRef run;
    std::string detail;
    bool definitelyUnsent = false;
};
struct ReadRequest {
    std::string environmentId, threadId, runId, itemId, cursor, view = "messages";
    std::size_t offset = 0;
    Json config = Json::object();
};
struct Excerpt {
    std::string text, threadId, runId, itemId, nextCursor;
    std::size_t nextOffset = 0;
    bool more = false;
};
class T3 {
public:
    virtual ~T3() = default;
    virtual std::string authGeneration(const std::string& environmentId, const Json& config) = 0;
    // Local/preflight checks only. Throw ModelError for a settings failure and report auth separately.
    virtual std::optional<Delivery> preflight(const Command&) { return std::nullopt; }
    virtual Delivery send(const Command&) = 0;
    virtual Delivery reconcile(const Command&) = 0;
    virtual Observation observe(const std::string& environmentId, const Json& config, const RunRef&) = 0;
    virtual void interrupt(const std::string& environmentId, const Json& config, const RunRef&) = 0;
    virtual Excerpt read(const ReadRequest&) { throw std::runtime_error("Session reading is unavailable"); }
};

// Store methods use SQLite transactions and may be called through separate CLI/Web connections.
class Store {
public:
    explicit Store(std::filesystem::path database);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Id submit(const Intake&);
    // Enqueues a Bot message for Runtime. Retrying the same key and payload returns the same receipt.
    Json receive(const Intake& routing, std::string_view clientMessageId, std::string_view text,
                 Id replyToMessageId = 0);
    std::optional<Json> receipt(std::string_view botId, std::string_view clientMessageId,
                                std::string_view text, Id replyToMessageId = 0) const;
    Json clarifications(std::string_view botId) const;
    // Operations: answer {text}, approve, retry, extend {deadline}, reconcile
    // {threadId?,runId?,questionId?,session?}, cancel. All except cancel require a matching waitId.
    void operate(Id taskId, Id waitId, std::string_view operation, const Json& data = Json::object());
    Json task(Id taskId) const;
    bool hasTask(Id taskId) const;
    // tasks: newest first, cursor is an exclusive id. history: oldest first, cursor is an exclusive id.
    Json tasks(std::string_view botId = {}, Id before = 0, unsigned limit = 50) const;
    Json activeTasks(std::string_view botId) const;
    Json sessionTasks(std::string_view botId, Id before, unsigned limit) const;
    Json history(Id taskId, Id after = 0, unsigned limit = 50) const;
    // Latest completion or Session report for the Task's current revision.
    Json latestTaskResult(Id taskId) const;
    // One page of chronological user-visible conversation, across this Bot's tasks.
    Json conversation(std::string_view botId, Id before = 0, unsigned limit = 50) const;
    // The same chronological page from either side of a Bot pair, with one row per message.
    Json peerConversation(std::string_view bot, std::string_view peer, Id before = 0, unsigned limit = 50) const;
    std::vector<std::string> peerPartners(std::string_view bot) const;
    std::optional<Json> peerReceipt(std::string_view bot, std::string_view peer,
                                    std::string_view clientMessageId, std::string_view text) const;
    // The caller resolves target using the contextual Bot's directional peer permissions.
    Json receivePeer(std::string_view contextBot, const Intake& target,
                     std::string_view clientMessageId, std::string_view text);
    const std::filesystem::path& path() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    friend class Runtime;
};

struct RuntimeOptions {
    std::function<Seconds()> clock = unixTime;
    // "off", "on_demand", "prefill". Prefill uses the same bounded T3 history reader.
    std::string sessionRead = "off";
    // Resolve the peers this sender can use, with each peer's own execution settings.
    std::function<std::vector<PeerBot>(std::string_view)> peers;
};
class Runtime {
public:
    using NotesLoader = std::function<BotNotes(std::string_view)>;
    Runtime(Store&, Model&, T3&, NotesLoader, RuntimeOptions = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    void routeConversation();
    // tick() completes a step for each eligible project. tickConcurrent() schedules
    // eligible projects independently and returns immediately.
    void tick();
    void tickConcurrent();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace flockv0
