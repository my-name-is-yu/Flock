#pragma once

#include "flock/root_delivery.hpp"
#include "flock/t3_connection.hpp"

namespace flockv0 {

struct HandoffAck {
    Id channel;
    std::string requestId;
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
    SessionHandoff(Store& store, ThreadSession& session, People people, Runtime* runtime = nullptr,
                   Peers peers = {})
        : store_(store), session_(session), people_(std::move(people)), runtime_(runtime), peers_(std::move(peers)) {}

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
    void consult(const Person&, Id channel, Model&, const IntakeResolver&, const Runtime::SoulLoader&);
    SessionHandoffView step(const Person&, Id channel, Model&, const IntakeResolver&, const Runtime::SoulLoader&);
    std::vector<std::pair<Id,std::string>> pending(Id after) const;

private:
    HandoffAck openChannel(const T3Connection&, const ThreadProof&, std::string_view bot,
                           std::string_view requestId, std::string_view text, std::string_view payloadDigest,
                           const std::optional<CoordinationRequest>&);
    SessionHandoffView freezeAs(const Person&, Id channel, std::string_view state);
    Store& store_;
    ThreadSession& session_;
    People people_;
    Runtime* runtime_;
    Peers peers_;
};

}
