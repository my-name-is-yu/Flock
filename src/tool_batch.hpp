#pragma once

#include "flock/types.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
struct Decision;
}

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

// Splits one model response into the steps that run it. Calls that can share a batch run first in one step,
// then each remaining call runs alone in the order given. Each entry is an index into tools.
std::vector<std::vector<std::size_t>> planSteps(std::span<const std::string> tools);

std::string turnCallKey(Id message, int step, std::optional<std::size_t> index = std::nullopt);
}
