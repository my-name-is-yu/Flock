#pragma once

#include "flock/core.hpp"

namespace flockv0 {
enum class OutboxPhase { ready, started, accepted, closed, superseded };

struct OutboxRunIdentity {
    std::string threadId, runId;
};

struct OutboxStopApproval {
    Decision decision;
    Id generation, revisionId;
    std::string scope, requester;
    Id anchor, turn;
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
    std::optional<Id> answerEventId, sourceMessageId;
    std::optional<std::string> peerMessageId;
};

struct OutboxSourceRun {
    std::string threadId, runId;
    std::optional<std::string> environmentId, gatewayEnvironmentId, link, itemId, status;
    std::optional<RunAssignment> assignment;
};

struct OutboxSource {
    Id taskId, messageId;
    std::optional<Id> revisionId;
    std::optional<std::string> text, stageId, artifactVersion, resultRunId;
    std::optional<OutboxSourceRun> run;
};

struct OutboxInstruction {
    Id messageId;
    int step;
    std::string text;
    std::optional<Id> revisionId;
    bool conflicting;
    std::optional<OutboxSource> source;
};

struct SessionMemory {
    Id version;
    std::string section;
    std::size_t excerptBytes;
};

struct Outbox {
    CommandPayload command;
    OutboxPhase phase = OutboxPhase::ready;
    std::string authGeneration;
    std::optional<Id> revisionId;
    std::optional<std::string> instructionBase;
    std::vector<OutboxInstruction> instructions;
    OutboxContext contextIds;
    std::vector<OutboxSource> sourceResultRefs;
    bool chargesBudget = false;
    bool destinationAction = false;
    std::optional<OutboxCommit> commit;
    std::optional<Id> sessionParticipationId;
    std::optional<SessionMemory> sessionMemory;
    std::optional<OutboxStop> stop;
};
}
