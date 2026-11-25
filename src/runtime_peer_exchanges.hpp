#pragma once

#include "runtime_components.hpp"
#include "storage.hpp"
#include "journal_peer.hpp"
#include "journal_task.hpp"
#include "flock/types.hpp"
#include "flock/intake.hpp"
#include "flock/store.hpp"
#include "flock/runtime.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Runtime::Components::PeerExchanges {
public:
    struct PeerResult {
        Id taskId;
        std::string kind, environment;
    };

    PeerExchanges(Store::Impl&, const Policy&, TaskUpdates&, RunReports&, SessionHistory&, const PeerResolver&);
    std::optional<PeerResult> peerResult(const TaskData&, const Json& event) const;
    std::vector<PeerBot> peersFor(std::string_view bot) const;
    PeerBot allowedPeer(std::string_view sender, std::string_view botId) const;
    std::optional<ForwardedPeerMessage> forwardableUserMessage(const TaskData&) const;
    void sendPeer(TaskData& sender, const Json& event, const TaskDecision&, PeerBot, const std::string& action);
    void finishPeer(const PeerExchange&, TaskData& recipient, TaskData sender, std::string_view status, std::string_view text,
                    const std::string& action);
    Json replyKinds(const TaskData&, const PeerExchange&) const;
    bool replyPeer(TaskData&, const Json& event, const TaskDecision&, const std::string& action);
    void readPeerEvidence(const TaskData&, const Json& event, const TaskDecision&);
    std::optional<PeerCompletion> peerCompletion(const TaskData&, const Json& event);
    bool forwardedQuestionAnswer(const TaskData&, const QuestionTarget&);
private:
    static BotMessageEntry peerMessage(const PeerExchange&, const TaskData& from, const TaskData& to, std::string_view type,
                                       std::string_view body, std::string_view messageId);
    void transitionPeer(const PeerExchange&, TaskData& actor, TaskData& task, const BotMessageEntry&,
                        std::optional<Id> forwardedSource = {});

    Store::Impl& db;
    const Policy& policy;
    TaskUpdates& updates;
    RunReports& reports;
    SessionHistory& history;
    const PeerResolver& resolvePeers;
};
}
