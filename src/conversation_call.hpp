#pragma once

#include "flock/bot_change.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"
#include <functional>
#include <stop_token>

namespace flockv0 {
class GitHost;
class T3;

bool hasDestinationSelector(const Json&);

enum class CallOutcome { advance, waiting, stop, finished };
struct ConversationCallContext {
    T3& t3;
    const std::function<Seconds()>& clock;
    GitHost* gitHost;
    const ApplyBotChange& applyBotChange;
    const ReconcileBotCreation& reconcileBotCreation;
    const IntakeResolver& intake;
    const PeerResolver& peers;
    std::stop_token shutdown;
    bool hasWebEvidence;
};
}
