#pragma once

#include "flock/core.hpp"

#include <span>

namespace flockv0::tool_batch {
enum class CallClass { parallel_read, gated_read, exclusive, terminal };
enum class ConversationRead { none, store, session, environment_readiness };

CallClass classify(const Decision&);
ConversationRead conversationRead(std::string_view tool);
bool batchable(std::string_view tool);

struct Candidate {
    std::size_t slot;
    const Decision& decision;
};

struct PlannedCall {
    std::size_t slot;
    std::vector<std::size_t> predecessors;
};

std::vector<std::vector<PlannedCall>> planWaves(std::span<const Candidate> validatedCalls);

std::string turnCallKey(Id message, int step, std::optional<std::size_t> index = std::nullopt);
}
