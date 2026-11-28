#pragma once

#include "flock/outbox_instruction.hpp"
#include "flock/outbox.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace flockv0 {
inline constexpr std::string_view runOutboxOperation = "run-outbox/";
inline constexpr std::string_view runStopOperation = "run-stop/";

class Database;
class RecordDecoder;
class SecretRedactor;
class Statement;
struct TaskData;

Json attachmentRefsJson(const std::vector<TaskAttachmentRef>&);
std::vector<TaskAttachmentRef> decodeAttachmentRefs(const Json&, const RecordDecoder&);

class OutboxStorage {
public:
    explicit OutboxStorage(Database& db) : db_(db) {}
    std::optional<Outbox> outbox(const TaskData&, std::string_view actionId = {}) const;
    void insertOutbox(Id task, const Outbox&);
    bool saveOutbox(const Outbox&, std::optional<OutboxPhase> expected = {});
    bool setOutboxPhase(std::string_view id, OutboxPhase, std::optional<OutboxPhase> expected = {});
    bool startOutbox(const Outbox&, std::string_view authGeneration);
    bool prepareOutbox(const Outbox&);
private:
    Database& db_;
};

std::string_view outboxPhaseName(OutboxPhase);
Json outboxActionJson(const OutboxAction&);
Json outboxColumnValues(const Outbox&);
Outbox decodeOutboxRow(const Statement&);
std::vector<OutboxInstruction> taskInstructions(const Json&, Id task, std::string_view key = "pendingInstructions");
Json instructionJson(const OutboxInstruction&);
OutboxInstruction decodeOutboxInstruction(const Json&, const RecordDecoder&);
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
