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
struct ModelCall;
}

namespace flockv0::tool_batch {
enum class CallClass { parallel_read, gated_read, exclusive, terminal };
enum class ConversationRead { none, store, session, environment_readiness };

CallClass classify(const Decision&);
ConversationRead conversationRead(std::string_view tool);
bool batchRead(const Decision&);

using StepCallIndexes = std::vector<std::size_t>;
std::vector<StepCallIndexes> planSteps(std::span<const ModelCall> calls);

std::string turnCallKey(Id message, int step, std::optional<std::size_t> index = std::nullopt);
}
