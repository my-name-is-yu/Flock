#pragma once

#include "flock/run_ref.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
enum class CommandTool { none, delegate, start_in_session, follow_up, answer_session_question, message_session };
std::string_view commandToolName(CommandTool);
std::optional<CommandTool> commandToolFromName(std::string_view);

struct CommandPayload {
    std::string actionId, environmentId, projectId, instruction;
    CommandTool tool = CommandTool::none;
    nlohmann::json answers = nlohmann::json::object();
    RunRef target;
    std::string questionId;
    std::optional<RunRef> candidate;
    std::string candidateQuestionId;
    std::string launchReceiptThreadId;
    std::string gatewayEnvironmentId;
};
}
