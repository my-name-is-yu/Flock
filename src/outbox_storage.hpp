#pragma once

#include "flock/outbox.hpp"

namespace flockv0 {
std::string_view outboxPhaseName(OutboxPhase);
Json to_json(const Outbox&);
Outbox from_json(std::string_view id, std::string_view phase, std::string_view authGeneration, std::string_view data);
std::vector<OutboxInstruction> taskInstructions(const Json&, Id task);
std::vector<OutboxSource> taskResultReferences(const Json&, Id task);
Json sourceJson(const OutboxSource&);
Json stopPolicyJson(const OutboxStopApproval&);
}
