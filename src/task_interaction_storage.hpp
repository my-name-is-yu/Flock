#pragma once

#include "flock/core.hpp"

namespace flockv0 {
class RecordDecoder;
class SecretRedactor;

void decodeTaskInteraction(TaskData&, const Json&, const RecordDecoder&);
void encodeTaskInteraction(Json&, const TaskData&);
Json questionJson(const Question&);
Json questionTargetJson(const QuestionTarget&);
QuestionTarget waitQuestionTarget(const Wait&);
void redactQuestion(Question&, const SecretRedactor&);
}
