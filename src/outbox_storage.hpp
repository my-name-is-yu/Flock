#pragma once

#include "flock/core.hpp"
#include "flock/outbox.hpp"

namespace flockv0 {
class RecordDecoder;
class SecretRedactor;
std::string_view outboxPhaseName(OutboxPhase);
Json outboxActionJson(const OutboxAction&);
Json to_json(const Outbox&);
Outbox from_json(std::string_view id, std::string_view phase, std::string_view authGeneration, std::string_view data);
std::vector<OutboxInstruction> taskInstructions(const Json&, Id task, std::string_view key = "pendingInstructions");
Json instructionJson(const OutboxInstruction&);
Json instructionsJson(std::span<const OutboxInstruction>);
void redactInstructions(std::span<OutboxInstruction>, const SecretRedactor&);
std::vector<OutboxSource> taskResultReferences(const Json&, Id task);
OutboxSource decodeOutboxSource(const Json&, const RecordDecoder&);
OutboxSourceRun decodeOutboxSourceRun(const Json&, const RecordDecoder&);
Json sourceJson(const OutboxSource&);
Json sourceRunJson(const OutboxSourceRun&);
void redactSource(OutboxSource&, const SecretRedactor&);
Json stopPolicyJson(const OutboxStopApproval&);
}
