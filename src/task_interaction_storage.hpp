#pragma once

#include "flock/task_interaction.hpp"
#include "flock/json.hpp"
#include "flock/wait.hpp"

namespace flockv0 {
struct TaskData;
class RecordDecoder;
class SecretRedactor;

void decodeTaskInteraction(TaskData&, const Json&, const RecordDecoder&);
void encodeTaskInteraction(Json&, const TaskData&);
Json questionJson(const Question&);
Json questionTargetJson(const QuestionTarget&);
QuestionTarget waitQuestionTarget(const Wait&);
void redactQuestion(Question&, const SecretRedactor&);
}
