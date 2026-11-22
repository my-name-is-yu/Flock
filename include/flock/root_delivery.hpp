#pragma once

#include "flock/participation.hpp"

namespace flockv0 {

struct ThreadEvent {
    std::string id, messageId, runId, kind, status, createdBy, text;
    Id position;
    std::string creationSource;
};
struct ThreadPage {
    ThreadTarget target;
    Id cursor;
    std::string authGeneration, activeRunId;
    std::vector<ThreadEvent> events;
};
struct ThreadReceipt {
    std::string messageId, runId, mode;
};
struct ThreadSend {
    enum Kind { Accepted, Rejected, Unknown };
    Kind kind = Unknown;
    std::optional<ThreadReceipt> receipt;
};
class ThreadSession {
public:
    virtual ~ThreadSession() = default;
    virtual std::optional<ThreadPage> read(const ThreadTarget&, std::optional<Id> after) = 0;
    virtual std::optional<ThreadPage> dispatchHistory(const ThreadTarget&, Id) { return {}; }
    virtual ThreadSend send(const ThreadTarget&, std::string_view authGeneration,
                            std::string_view requestId, std::string_view text) = 0;
    virtual std::optional<std::string> inputCommit(const ThreadTarget&) = 0;
};

enum class RootDeliveryState { Prepared, Dispatching, Accepted, Queued, Observed, Failed, Rejected, Unknown };
std::string_view rootDeliveryStateName(RootDeliveryState);

struct RootDeliveryView {
    std::string commandId;
    RootDeliveryState state;
    std::optional<ThreadReceipt> receipt;
    std::string observedRunId;
};

class RootDelivery {
public:
    RootDelivery(Store& store, ThreadSession& session) : store_(store), session_(session) {}
    void prepare(Id channel, Id generation, std::string_view commandId, std::string_view bot,
                 std::string_view requestId, std::string_view text, Id observationCursor);
    RootDeliveryView deliver(const Person&, Id channel, std::string_view commandId);
    void reconcile(const Person&, Id channel);
private:
    Store& store_;
    ThreadSession& session_;
};

}
