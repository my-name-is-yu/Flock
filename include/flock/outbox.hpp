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
enum class OutboxPhase { ready, started, accepted, closed, superseded, unconfirmed };

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
};

struct OutboxContext {
    std::optional<std::int64_t> answerEventId, sourceMessageId;
};

template <typename Version> struct SessionContextSection {
    Version version;
    std::string section;
    std::size_t excerptBytes;
};

using SessionRole = SessionContextSection<std::string>;
using SessionMemory = SessionContextSection<std::int64_t>;

struct SessionContext {
    std::optional<SessionRole> role;
    std::optional<SessionMemory> memory;
};

struct OutboxAction {
    CommandPayload command;
    OutboxPhase phase = OutboxPhase::ready;
    std::optional<std::int64_t> revisionId;
    std::optional<std::string> instructionBase;
    std::vector<OutboxInstruction> instructions;
    std::vector<TaskAttachmentRef> attachments{};
    OutboxContext contextIds;
    std::vector<OutboxSource> sourceResultRefs;
    bool destinationAction = false;
    std::optional<OutboxCommit> commit;
    std::optional<std::int64_t> sessionParticipationId;
    std::optional<OutboxStop> stop;
};

struct Outbox : OutboxAction {
    std::string authGeneration;
    SessionContext sessionContext;
};
}
