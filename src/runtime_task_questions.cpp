#include "database.hpp"
#include "task_interaction_storage.hpp"
#include "wait_storage.hpp"
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
void runtime::TaskQuestions::closeQuestion(TaskData& task, std::string_view reason, std::string_view resolution) {
    waits.clearWait(task, reason);
    waits.unlockQuestion(task, resolution.empty() ? reason : resolution);
}

void runtime::TaskQuestions::suspendQuestion(TaskData& task) {
    if (!task.question) return;
    auto& question = *task.question;
    if (const auto wait = waits.currentWait(task.id)) {
        const auto reason = wait->reason();
        if (reason == WaitReason::user_question || reason == WaitReason::bot_notes_unavailable) question.suspendedWaitId = wait->id;
    }
}

void runtime::TaskQuestions::resumeQuestion(TaskData& task) {
    auto& question = *task.question;
    const auto& target = question.target;
    const auto waitId = std::exchange(question.suspendedWaitId, std::nullopt);
    Id restoredQuestion = 0;
    waits.clearWait(task, "connection_restored");
    task.state = TaskState::deciding;
    if (waitId) {
        auto row =
            database.query("SELECT id,reason,data FROM waits WHERE id=? AND task_id=? AND resolution='superseded'", {*waitId, task.id});
        if (row.next()) {
            auto wait = decodeWaitRow(row);
            const auto reason = wait.reason();
            const auto* asked = std::get_if<ModelQuestionWait>(&wait.details);
            if (wait.eventId == target.eventId && wait.run == target.run &&
                (reason != WaitReason::user_question || (asked && asked->questionId == target.requestId))) {
                const auto restored = waits.wait(task, std::move(wait));
                if (reason == WaitReason::user_question) restoredQuestion = restored;
            }
        }
    }
    question.target = questionTarget(task, restoredQuestion);
}
}
