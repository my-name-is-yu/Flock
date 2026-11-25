#pragma once

#include "flock/command.hpp"
#include "flock/decision.hpp"
#include "flock/outbox_instruction.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flockv0 {
enum class OutboxPhase { ready, started, accepted, closed, superseded };

struct OutboxRunIdentity {
    std::string threadId, runId;
};

struct OutboxStopApproval {
    Decision decision;
    std::int64_t generation, revisionId;
    std::string scope, requester;
    std::int64_t anchor, turn;
    bool background;
};

struct OutboxStop {
    std::string operation;
    std::optional<OutboxRunIdentity> run;
    std::optional<OutboxStopApproval> approval;
};

struct OutboxCommit {
    std::string expected;
    bool markedChecks;
};

struct OutboxContext {
    std::optional<std::int64_t> answerEventId, sourceMessageId;
    std::optional<std::string> peerMessageId;
};

struct SessionMemory {
    std::int64_t version;
    std::string section;
    std::size_t excerptBytes;
};

struct OutboxAction {
    CommandPayload command;
    OutboxPhase phase = OutboxPhase::ready;
    std::optional<std::int64_t> revisionId;
    std::optional<std::string> instructionBase;
    std::vector<OutboxInstruction> instructions;
    OutboxContext contextIds;
    std::vector<OutboxSource> sourceResultRefs;
    bool chargesBudget = false;
    bool destinationAction = false;
    std::optional<OutboxCommit> commit;
    std::optional<std::int64_t> sessionParticipationId;
    std::optional<OutboxStop> stop;
};

struct Outbox : OutboxAction {
    std::string authGeneration;
    std::optional<SessionMemory> sessionMemory;
};
}
