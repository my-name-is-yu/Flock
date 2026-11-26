#include "runtime_session_prompts.hpp"
#include "wait_storage.hpp"
#include "task_storage.hpp"
#include "workflow_storage.hpp"
#include "flock/limits.hpp"
#include "journal.hpp"
#include "journal_task.hpp"
#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "runtime_task_state.hpp"
#include "task_context_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/t3_command.hpp"
#include "flock/task.hpp"
#include "flock/outbox.hpp"
#include "flock/outbox_instruction.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "json_fields.hpp"
#include "secret_storage.hpp"
#include "task_events.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
struct SourceQuotes {
    const SecretRedactor& scrub;
    std::size_t remaining = limits::session_prompt::maxQuoteTextBytes;
    std::set<Id> included{};

    std::string quote(const OutboxSource& source, std::size_t limit = limits::session_prompt::maxQuoteTextBytes) {
        const auto id = source.messageId;
        if (!id || !included.insert(id).second) return {};
        auto text = "\n\nReferenced result " + std::to_string(id) + " from task " + std::to_string(source.taskId);
        if (source.stageId) text += "\nStage: " + *source.stageId;
        if (source.run) text += "\nSource Run: " + sourceRunJson(*source.run).dump();
        if (source.resultRunId) text += "\nReport Run: " + *source.resultRunId;
        text += " (reference only):";
        const auto excerpt = bounded(scrub.text(source.text.value_or("")), std::min(remaining, limit));
        remaining -= excerpt.size();
        if (!excerpt.empty()) text += "\n" + excerpt;
        return scrub.text(text);
    }
};

std::vector<OutboxInstruction> pendingBatch(std::span<const OutboxInstruction> pending, std::size_t promptBudgetBytes,
                                            SourceQuotes quotes) {
    std::vector<OutboxInstruction> batch;
    std::size_t serializedBytes = 0, promptBytes = 0;
    for (const auto& item : pending) {
        serializedBytes += instructionJson(item).dump().size();
        promptBytes += 2 + item.text.size();
        if (item.source) promptBytes += quotes.quote(*item.source).size();
        if (!batch.empty() && (serializedBytes > limits::session_prompt::pendingBatchJsonThresholdBytes || promptBytes > promptBudgetBytes))
            break;
        batch.push_back(item);
    }
    return batch;
}

void includeInstructions(std::string& instruction, std::span<const OutboxInstruction> pending) {
    if (pending.empty()) return;
    instruction += "\n\nUser instructions accepted for this task, oldest first. Apply later directions where they replace earlier ones:";
    for (const auto& item : pending) instruction += "\n\n" + item.text;
}
}

Runtime::Components::SessionPrompts::SessionContract Runtime::Components::SessionPrompts::sessionContract(const TaskData& task) {
    auto row = database.query(R"SQL(
SELECT r.intent,r.done_when,
       coalesce((SELECT max(j.id) FROM journal j WHERE j.task_id=r.task_id AND
         ((r.number=1 AND j.kind='task_contract_changed') OR
          (r.number>1 AND j.kind='task_revision' AND j.revision_id=r.id))),
         r.source_message_id,0)
FROM task_revisions r WHERE r.id=? AND r.task_id=?
)SQL",
                              {revisionJson(task.currentRevision), task.id});
    if (!row.next()) throw Conflict("The new Session needs an established task revision");
    return {row.string(0), row.string(1), row.number(2)};
}

std::vector<OutboxSource> Runtime::Components::SessionPrompts::appendSessionDirections(std::string& instruction, const TaskData& task,
                                                                                       std::span<const OutboxInstruction> pending,
                                                                                       Id answersAfter) {
    using InstructionId = std::pair<Id, int>;
    std::set<InstructionId> pendingIds;
    for (const auto& item : pending) pendingIds.emplace(item.messageId, item.step);
    auto rows = database.query(R"SQL(
SELECT j.id,j.kind,j.data,w.id,w.reason,w.data,
       CASE j.kind WHEN 'instruction_accepted' THEN EXISTS (
         SELECT 1 FROM journal d WHERE d.task_id=j.task_id AND d.kind='instruction_delivered'
           AND d.message_id=j.message_id AND d.instruction_step=j.instruction_step AND d.revision_id=?2)
       ELSE 0 END
FROM journal j
LEFT JOIN waits w ON j.kind='user_answer' AND w.id=j.wait_id AND w.task_id=j.task_id
WHERE j.task_id=?1 AND
      (j.kind='instruction_accepted' OR
       (j.kind='user_answer' AND j.id>?3 AND w.resolution='answer'))
ORDER BY j.id
)SQL",
                               {task.id, revisionJson(task.currentRevision), answersAfter});
    std::vector<OutboxSource> sources;
    bool first = true;
    while (rows.next()) {
        const auto recordId = rows.number(0);
        const bool isAnswer = rows.string(1) == "user_answer";
        const auto accepted = isAnswer ? std::nullopt : std::optional{decodeAcceptedInstruction(recordId, rows.json(2))};
        if (accepted && !rows.number(6) && !pendingIds.contains({accepted->messageId, accepted->step})) continue;
        if (std::exchange(first, false))
            instruction += "\n\nUser decisions for this task, oldest first. Apply later directions where they replace "
                           "earlier ones, under the current task intent and completion criteria above.";
        instruction += isAnswer ? "\n\nUser answer" : "\n\nAccepted instruction";
        instruction += " (record " + std::to_string(recordId);
        if (accepted) {
            instruction +=
                ", message " + std::to_string(accepted->messageId) + ", step " + std::to_string(accepted->step) + "):\n" + accepted->text;
        } else {
            const auto answer = decodeUserAnswer(recordId, rows.json(2));
            if (answer.messageId) instruction += ", message " + std::to_string(*answer.messageId);
            instruction += ", wait " + std::to_string(answer.waitId) + "):\n";
            instruction += decodeWaitRow(rows, 3).text + "\nAnswer: " + answer.text;
        }
        if (instruction.size() > limits::session_prompt::maxBytes) throw ContextLimit{};
        if (accepted && accepted->source) sources.push_back(*accepted->source);
    }
    return sources;
}

Runtime::Components::SessionPrompts::WorkPrompt Runtime::Components::SessionPrompts::workPrompt(const TaskData& task, const Json& event,
                                                                                                std::size_t actionIdBytes,
                                                                                                const SessionContract* contract) {
    WorkPrompt prompt;
    auto& [instruction, pending, contextIds, resultReferences] = prompt;
    const auto scrub = secretStorage.redactor(task.botId);
    std::size_t reservedBytes =
        limits::session_prompt::instructionReserveBytes + std::max(limits::session_prompt::minActionIdReserveBytes, actionIdBytes);
    if (!contract && event.contains("userAnswer")) {
        contextIds.answerEventId = event.at("id").get<Id>();
        instruction += "\n\nUser answer:\n" + field(event, "userAnswer");
    }
    const bool moved = task.newSessionRequired;
    if (contract || moved || !task.lastRun || task.lastRun->assignment.revisionId != task.currentRevision) {
        const auto criteria = contract ? contract->doneWhen : effectiveDoneWhen(task);
        instruction += "\n\nTask intent:\n" + (contract ? contract->intent : task.intent) + "\n\nCompletion criteria:\n" + criteria;
        reservedBytes += std::max(criteria.size(), limits::session_prompt::maxDecisionTextBytes) - criteria.size();
    }
    if (contract || moved) {
        if (field(event, "kind") == "bot_answer") {
            contextIds.peerMessageId = event.at("messageId").get<std::string>();
            instruction += "\n\nAnswer from the requesting Bot:\n" + field(event, "text");
        }
    }
    if (task.sourceTaskId && !task.lastRun) {
        contextIds.sourceMessageId = task.sourceMessageId;
        instruction += "\n\nUser request:\n" + task.request;
    }
    if (const auto& restart = task.restartContext; restart && !task.lastRun) {
        const auto& delivered = restart->deliveredInstructions;
        if (!delivered.empty()) instruction += "\n\nEarlier user instructions delivered before cancellation (reference only):";
        for (const auto& item : delivered) instruction += "\n\n" + item.text;
    }
    SourceQuotes quotes{scrub};
    const auto quoteSource = [&](OutboxSource source, std::size_t limit = limits::session_prompt::maxQuoteTextBytes) {
        redactSource(source, scrub);
        const auto quote = quotes.quote(source, limit);
        if (quote.empty()) return;
        instruction += quote;
        source.text.reset();
        resultReferences.push_back(std::move(source));
    };
    constexpr std::size_t recentReportLimit = 3, reportExcerptBytes = 2000;
    const auto recentReportBudget = moved ? quotes.remaining / 2 : 0;
    quotes.remaining -= recentReportBudget;
    if (task.sourceTaskId && (moved || !task.lastRun)) {
        for (const auto& source : task.sourceResultRefs)
            if (source.taskId == *task.sourceTaskId) quoteSource(source);
    }
    if (task.workflow) {
        for (const auto& source : task.sourceResultRefs) quoteSource(source);
        workflowStorage.appendStageRequirements(instruction, task);
    }
    quotes.remaining += recentReportBudget;
    if (moved) {
        auto reports = database.query("SELECT id,data FROM journal WHERE task_id=? AND kind='report' ORDER BY id DESC LIMIT ?",
                                      {task.id, recentReportLimit});
        auto remainingReports = recentReportLimit;
        while (reports.next()) {
            const auto report = decodeRunReport(reports.number(0), reports.json(1));
            OutboxSource source{.taskId = task.id,
                                .messageId = reports.number(0),
                                .text = report.evidence.report,
                                .resultRunId = report.resultRunId,
                                .run = reportSourceRun(report)};
            quoteSource(std::move(source), std::min(reportExcerptBytes, quotes.remaining / remainingReports));
            --remainingReports;
        }
    }
    const bool resumedTask = task.restartContext && !task.lastRun;
    if (contract && !resumedTask)
        pending = task.pendingInstructions;
    else {
        constexpr auto maxPromptBytes = limits::session_prompt::maxBytes;
        const auto occupiedBytes = reservedBytes + instruction.size();
        pending = pendingBatch(task.pendingInstructions, maxPromptBytes - std::min(maxPromptBytes, occupiedBytes), quotes);
    }
    if (contract) {
        const auto sources = appendSessionDirections(instruction, task, pending, contract->answersAfter);
        if (resumedTask && pending.size() < task.pendingInstructions.size())
            instruction += "\n\nFlock will send the remaining inherited instructions in follow-up Runs in this Session. "
                           "Work only on the instructions included in this Run until the rest arrive.";
        for (const auto& source : sources) quoteSource(source);
    } else {
        includeInstructions(instruction, pending);
        for (const auto& item : pending)
            if (item.source) quoteSource(*item.source);
    }
    return prompt;
}

void Runtime::Components::SessionPrompts::prepareDelegation(const TaskData& task, Outbox& action) {
    auto instruction = *action.instructionBase;
    const auto contract = sessionContract(task);
    const auto event = taskEvents.currentEvent(task);
    auto prompt = workPrompt(task, event, action.command.actionId.size(), &contract);
    const auto scrub = secretStorage.redactor(task.botId);
    instruction += prompt.instruction;
    action.command.instruction = scrub.text(instruction);
    redactInstructions(prompt.pending, scrub);
    action.instructions = std::move(prompt.pending);
    action.contextIds = std::move(prompt.contextIds);
    action.sourceResultRefs = std::move(prompt.resultReferences);
    action.instructionBase.reset();
}

std::string Runtime::Components::SessionPrompts::dispatchedWorkPrompt(const Outbox& action) {
    auto instruction = action.command.instruction;
    if (action.sessionMemory) instruction += action.sessionMemory->section;
    return sessionPrompt(action.command.actionId, instruction);
}

Runtime::Components::SessionPrompts::WorkPrompt Runtime::Components::SessionPrompts::workPrompt(const TaskData& task, const Json& event,
                                                                                                std::size_t actionIdBytes) {
    return workPrompt(task, event, actionIdBytes, nullptr);
}

std::vector<OutboxInstruction> Runtime::Components::SessionPrompts::pendingInstructions(const TaskData& task, const Json& event) {
    return workPrompt(task, event, 0).pending;
}
}
