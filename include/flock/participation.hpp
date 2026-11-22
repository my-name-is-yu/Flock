#pragma once

#include "flock/access.hpp"

#include <variant>

namespace flockv0 {

struct ThreadTarget {
    std::string environmentId, projectId, threadId;
    bool operator==(const ThreadTarget&) const = default;
};
struct InvitationRef {
    ThreadTarget target;
    std::string requestId, botId;
    bool operator==(const InvitationRef&) const = default;
};
struct CoordinationRequest {
    std::string goal;
    bool allowDelegation = false;
};
struct ImmutableOrigin {
    InvitationRef invitation;
    std::string personId;
};
struct ReturnChannel {
    Id id;
    ImmutableOrigin origin;
    std::string request;
};
struct ConsultPermit {
    ReturnChannel channel;
    std::string commandId;
};
struct CoordinationBinding {
    CoordinationRequest request;
};
struct CoordinationActivation {
    ReturnChannel channel;
    CoordinationBinding binding;
};
using Participation = std::variant<ConsultPermit, CoordinationActivation>;

}
