#include "runtime_task_questions.hpp"
#include "record_decode.hpp"
#include "runtime_task_updates.hpp"
#include "flock/types.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/wait.hpp"

#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace flockv0 {
void Runtime::Components::TaskQuestions::closeQuestion(TaskData& task, std::string_view reason, std::string_view resolution) {
    db.clearWait(task, reason);
    db.unlockQuestion(task, resolution.empty() ? reason : resolution);
}

void Runtime::Components::TaskQuestions::suspendQuestion(TaskData& task) {
    if (!task.question) return;
    auto& question = *task.question;
    if (const auto wait = db.currentWait(task.id)) {
        const auto reason = wait->reason();
        if (reason == WaitReason::user_question || reason == WaitReason::model_budget || reason == WaitReason::model_failure)
            question.suspendedWaitId = wait->id;
    }
    const auto& pending = task.pendingDecision;
    if (pending && TaskUpdates::checkpointMatches(*pending, task)) question.suspendedDecision = *pending;
}

std::optional<DecisionCheckpoint> Runtime::Components::TaskQuestions::resumeQuestion(TaskData& task) {
    auto& question = *task.question;
    const auto& target = question.target;
    const auto waitId = std::exchange(question.suspendedWaitId, std::nullopt);
    Id restoredQuestion = 0;
    db.clearWait(task, "connection_restored");
    task.state = TaskState::deciding;
    if (waitId) {
        auto row = db.query("SELECT id,reason,data FROM waits WHERE id=? AND task_id=? AND resolution='superseded'", {*waitId, task.id});
        if (row.next()) {
            auto wait = decodeWaitRow(row);
            const auto reason = wait.reason();
            const auto* asked = std::get_if<ModelQuestionWait>(&wait.details);
            if (wait.eventId == target.eventId && wait.run == target.run &&
                (reason != WaitReason::user_question || (asked && asked->questionId == target.requestId))) {
                const auto restored = db.wait(task, std::move(wait));
                if (reason == WaitReason::user_question) restoredQuestion = restored;
            }
        }
    }
    auto decision = std::exchange(question.suspendedDecision, std::nullopt);
    question.target = questionTarget(task, restoredQuestion);
    if (decision && (decision->revisionId != task.currentRevision || decision->eventId != task.eventId)) return std::nullopt;
    return decision;
}
}
