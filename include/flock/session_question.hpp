#pragma once

#include "flock/json.hpp"

#include <span>
#include <string_view>

namespace flockv0 {
struct QuestionField;

// T3 trims ordinary input and labels without explicit values using JavaScript String.trim.
std::string_view trimSessionQuestionText(std::string_view text);
// Validate a model answer against the observed fields and encode it for Session submission.
Json sessionQuestionAnswers(std::span<const QuestionField> fields, const Json& answers);
}
