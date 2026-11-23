#pragma once

#include "flock/root_delivery.hpp"
#include "flock/t3_connection.hpp"

#include <exception>
#include <map>
#include <mutex>

namespace flockv0 {

struct HandoffAck {
    Id channel;
    std::string requestId;
};
struct PendingHandoff {
    Id channel;
    std::string personId;
    bool coordination;
};
struct SessionHandoffView {
    Id channel;
    ImmutableOrigin origin;
    bool coordination = false, frozen = false;
    std::string state;
    std::optional<CoordinationRequest> permission;
    Id generation = 1;
    std::vector<std::string> residualCommandIds;
    std::vector<RootDeliveryView> deliveries;
    Id eventCursor = 0;
    Json latestDecision = nullptr;
    Json pendingQuestion = nullptr;
    Json work = Json::array();
    std::vector<std::string> admittedChildIds;
};

class SessionHandoff {
public:
    using Peers = std::function<std::vector<PeerBot>(std::string_view)>;
    using People = std::function<std::optional<Person>(std::string_view)>;
    SessionHandoff(Store& store, ThreadSession& session, People people, IntakeResolver intake,
                   Runtime* runtime = nullptr, Peers peers = {})
        : store_(store), session_(session), people_(std::move(people)), intake_(std::move(intake)),
          runtime_(runtime), peers_(std::move(peers)) {}

    void recover();

    std::optional<Person> person(const T3Connection&) const;
    HandoffAck invoke(const T3Connection&, const Json& arguments,
                      const std::function<bool(std::string_view)>& botExists,
                      std::chrono::milliseconds proofTimeout = std::chrono::seconds(3));
    SessionHandoffView resume(const Person&, Id channel);
    SessionHandoffView pause(const Person&, Id channel);
    SessionHandoffView freeze(const Person&, Id channel);
    SessionHandoffView answer(const Person&, Id channel, Id generation, std::string_view eventId,
                              std::string_view text);
    SessionHandoffView answerWork(const Person&, Id channel, Id generation, std::string_view delegation,
                                  std::string_view stage, Id wait, std::string_view text);
    SessionHandoffView view(const Person&, Id channel) const;
    std::vector<SessionHandoffView> recent(const Person&) const;
    void consult(const Person&, Id channel, Model&, const Runtime::SoulLoader&);
    SessionHandoffView step(const Person&, Id channel, Model&, const Runtime::SoulLoader&);
    std::vector<PendingHandoff> pending(Id after) const;
    void discardModelDecision(Id channel);

private:
    struct ModelCommit {
        Id attempt, generation;
        std::string personId;
        bool coordination;
        Json usage;
        std::function<void()> save;
        std::function<std::optional<bool>(bool retry)> checkEvidence;
        std::exception_ptr failure;
        bool deferredForHistory = false;
    };
    bool commitModelDecision(const Person*, Id channel, bool coordination, bool retry = true);
    template <typename Prepare>
    void checkpointModelDecision(const Person&, Id channel, Id generation, Id attempt, bool coordination,
                                 std::string_view bot, Model&, const ModelInput&, Prepare);

    HandoffAck openChannel(const T3Connection&, const ThreadProof&, std::string_view bot,
                           std::string_view requestId, std::string_view text, std::string_view payloadDigest,
                           const std::optional<CoordinationRequest>&);
    SessionHandoffView freezeAs(const Person&, Id channel, std::string_view state);
    Intake resolveBot(std::string_view bot) const;
    Store& store_;
    ThreadSession& session_;
    People people_;
    IntakeResolver intake_;
    Runtime* runtime_;
    Peers peers_;
    mutable std::mutex modelMutex_;
    std::map<Id,ModelCommit> modelCommits_;
};

}
