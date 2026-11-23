#pragma once

#include "flock/core.hpp"

namespace flockv0::tool_batch {
enum class CallClass { parallel_read, gated_read, exclusive, terminal };

CallClass classify(const Decision&);
bool batchable(const Decision&);

// Conflict detection assumes tool arguments have already been validated.
bool conflict(const Decision&, const Decision&);
std::vector<std::vector<std::size_t>> planWaves(const std::vector<Decision>&);

// Bot tools already use indexed keys, even on single-call steps.
std::string turnCallKey(Id message, int step, std::optional<std::size_t> index = std::nullopt);
}
