#pragma once

#include "flock/core.hpp"

namespace flockv0::tool_batch {
enum class CallClass { parallel_read, gated_read, exclusive, terminal };

CallClass classify(const Decision&);
bool batchable(const Decision&);

bool validatedCallsConflict(const Decision&, const Decision&);
std::vector<std::vector<std::size_t>> planWaves(const std::vector<Decision>& validatedCalls);

std::string turnCallKey(Id message, int step, std::optional<std::size_t> index = std::nullopt);
}
