#pragma once

#include "runtime_components.hpp"
#include "storage.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/model_error.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/outbox_instruction.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
class Runtime::Components::TaskTurn {
public:
    TaskTurn(Store::Impl& db, Model& model, const SoulLoader& soul, const std::function<Seconds()>& clock, GitHost* gitHost,
             const std::string& sessionRead, std::stop_token stop, TaskUpdates& updates, RunReports& reports,
             SessionHistory& sessionHistory, PeerExchanges& peerExchanges, RunObserver& runs, SessionPrompts& prompts,
             TaskDecisions& decisions);

    void decide(const TaskData& task);
    void persistTaskInferences();
private:
    enum class FailureCause { Model, BotNotes, UnsupportedReasoningEffort };
    struct TaskInference {
        Id generation, eventId, attemptId;
        std::optional<Id> revisionId;
        Json usage = Json::object();
        std::variant<ModelCall, ModelError> outcome{};
    };

    std::vector<std::string> available(const TaskData& task, const Json& event, const std::optional<PeerExchange>& peer, bool canSend,
                                       bool canRead);
    void recordFailure(TaskData& task, const Json& event, std::string_view detail, bool transient, FailureCause cause);
    void failure(const TaskData& task, const Json& event, std::string_view detail, bool transient,
                 FailureCause cause = FailureCause::Model);
    std::optional<TaskDecision> persistTaskInference(Id id);
    std::optional<TaskDecision> inferDecision(const TaskData& task, Json event, const Json& history, const std::vector<std::string>& tools,
                                              std::span<const OutboxInstruction> pending, Json choices,
                                              const std::optional<PeerExchange>& peer);

    Store::Impl& db;
    Model& model;
    const SoulLoader& soul;
    const std::function<Seconds()>& clock;
    GitHost* gitHost;
    const std::string& sessionRead;
    std::stop_token stop;
    TaskUpdates& updates;
    RunReports& reports;
    SessionHistory& sessionHistory;
    PeerExchanges& peerExchanges;
    RunObserver& runs;
    SessionPrompts& prompts;
    TaskDecisions& decisions;
    std::map<Id, TaskInference> taskInferences;
};
}
