#include "demo.hpp"
#include "flock/command.hpp"
#include "flock/bot_setup.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/model_error.hpp"
#include "flock/run_ref.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/task_interaction.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0::app {
namespace {
constexpr std::string_view setupRequest = "Rename yourself to Release Scout and always run ./scripts/verify.sh before reporting.";
constexpr std::string_view fridayRequest = "Remember that we deploy on Fridays.";
constexpr std::string_view fridayMemory = "Deploy on Fridays.";
constexpr std::string_view verifyProjectRequest = "Show me how to verify this project";
constexpr std::string_view releaseSummaryRequest = "Implement the release summary in Demo project, then verify it in a separate Session.";
constexpr std::string_view verificationReport = "The demo project's full check runs with ./scripts/verify.sh.";
constexpr std::string_view verificationRunSuffix = "-verify-project";

class DemoModel final : public Model {
    ModelTurn decide(const ModelInput& in) override {
        const auto event = in.event.value("kind", "");
        if (event == "conversation_turn" && in.event.contains("sessionWake")) {
            const auto& task = in.event.value("task", Json::object());
            if (!task.empty() && in.event.at("turn").empty() && std::ranges::find(in.tools, "complete") != in.tools.end()) {
                const auto report = task.value("report", "");
                Json args{{"report", report.empty() ? "The demo finished its assigned work." : report}};
                if (!in.learningRunId.empty() && report == verificationReport)
                    args["learnings"] =
                        Json::array({{{"text", "./scripts/verify.sh runs the demo project's full check."}, {"runId", in.learningRunId}}});
                return {{{"complete", std::move(args), "The demo Session reported that it completed the requested work."}}};
            }
            return {{{"reply", {{"text", "The demo work finished."}}, {}}}};
        }
        if (event == "conversation_turn") {
            const auto request = in.event.value("text", "");
            const auto& turn = in.event.at("turn");
            const auto& reply = in.event.at("replyTo");
            if (request == releaseSummaryRequest && reply.is_null() && (turn.empty() || turn.back().at("result").value("ok", false))) {
                if (turn.size() >= 2)
                    return {{{"reply", {{"text", "I queued the implementation and a separate verification Session."}}, {}}}};
                const auto taskRequest = turn.empty() ? "Implement the release summary" : "Verify the release summary";
                return {{{"start_task", {{"request", taskRequest}, {"projectId", "demo-project"}}, {}}}};
            }
            if (!turn.empty()) {
                const auto& last = turn.back();
                const bool ok = last.at("result").value("ok", false);
                const auto tool = last.value("tool", "");
                std::string text = !ok                              ? "The demo couldn’t apply that request."
                                   : tool == "write_memory"         ? "I’ll remember that."
                                   : tool == "answer_task_question" ? "I passed your answer to the task."
                                   : tool == "read_result"          ? "The demo result records successful completion of simulated work. "
                                                                      "It contains no implementation details."
                                                                    : "I queued your request.";
                return {{{"reply", {{"text", text}}, {}}}};
            }
            if (request == setupRequest && std::ranges::find(in.tools, botSetupTool) != in.tools.end()) {
                auto role = in.notes.soul;
                if (!role.empty() && !role.ends_with('\n')) role += '\n';
                role += "Always run ./scripts/verify.sh before reporting.";
                return {{{std::string(botSetupTool), {{"name", "Release Scout"}, {"soul", std::move(role)}}, {}},
                         {"reply", {{"text", "I’m now Release Scout. I’ll run ./scripts/verify.sh before reporting."}}, {}}}};
            }
            if (request == fridayRequest && std::ranges::find(in.tools, "write_memory") != in.tools.end()) {
                auto content = in.notes.memory;
                if (content.find(fridayMemory) == content.npos) {
                    if (!content.empty() && !content.ends_with('\n')) content += '\n';
                    content.append(fridayMemory);
                }
                return {{{"write_memory",
                          {{"baseVersion", in.event.at("memoryVersion")}, {"content", content}, {"summary", fridayMemory}},
                          {}}}};
            }
            if (reply.is_object() && reply.value("kind", "") == "task_question" && reply.value("current", false)) {
                return {{{"answer_task_question",
                          {{"taskId", reply["taskId"]},
                           {"waitId", reply["waitId"]},
                           {"revisionId", reply["revisionId"]},
                           {"generation", reply["generation"]},
                           {"text", request}},
                          {}}}};
            }
            if (reply.is_object() && reply.value("kind", "") == "result")
                return {{{"read_result", {{"messageId", reply["messageId"]}}, {}}}};
            if (reply.is_object() && reply.value("kind", "") == "bot_message") {
                const auto source = reply.value("inReplyTo", Json::object()).value("replyToMessageId", Json(nullptr));
                if (!source.is_null()) return {{{"read_result", {{"messageId", source}}, {}}}};
            }
            return {{{"start_task", {{"request", request}}, {}}}};
        }
        if (event == "result" && in.task.contains("userInstructions")) {
            const bool follow = std::ranges::find(in.tools, "follow_up") != in.tools.end();
            return {{{follow ? "follow_up" : "delegate",
                      {{"instruction", "Apply the pending user instructions."}},
                      "The demo report predates the accepted instructions."}}};
        }
        if (event == "result" && std::ranges::find(in.tools, "complete") != in.tools.end()) {
            const auto report = in.event.value("report", "");
            Json args{{"report", report}};
            if (!in.learningRunId.empty() && report == verificationReport)
                args["learnings"] =
                    Json::array({{{"text", "./scripts/verify.sh runs the demo project's full check."}, {"runId", in.learningRunId}}});
            return {{{"complete", std::move(args), "The demo Session reported that it completed the requested work."}}};
        }
        if (std::ranges::find(in.tools, "delegate") != in.tools.end()) {
            std::string answer;
            for (const auto& row : in.history)
                if (row.value("kind", "") == "user_answer") answer = row.value("data", Json::object()).value("text", "");
            const auto request = in.task.value("request", "");
            if (answer.empty() && request.starts_with("ask:")) {
                auto question = request.substr(4);
                if (question.find_first_not_of(" \t\r\n") == std::string::npos) question = "What should the demo focus on?";
                return {{{"ask_user", {{"question", question}}, "The demo request calls for a user answer before work begins."}}};
            }
            Json args = {{"instruction", request + (answer.empty() ? "" : "\nUser answer: " + answer)}};
            if (const auto projects = in.task.find("projects"); projects != in.task.end()) {
                const auto& choice = answer.empty() ? request : answer;
                const auto selected = std::ranges::find_if(*projects, [&](const Json& project) {
                    const auto title = field(project, "title");
                    return !title.empty() && choice.find(title) != choice.npos;
                });
                if (selected == projects->end())
                    return {{{"ask_user",
                              {{"question", "Should I use Demo project or Demo documentation?"}},
                              "The request does not identify a project."}}};
                args["projectId"] = selected->at("projectId");
            }
            if (in.needsDoneWhen) args["doneWhen"] = "The demo Session returns a completion report.";
            return {{{"delegate", std::move(args), "A demo Session can handle this request."}}};
        }
        throw ModelError("No fixed demo decision for this event", false);
    }
};

class DemoT3 final : public T3 {
    std::string authGeneration(const std::string&, const Json&) override { return "fixed-demo"; }

    RunRef run(const Command& command) {
        const auto thread = command.tool == CommandTool::follow_up ? command.target.threadId : "demo-thread-" + command.actionId;
        auto runId = "demo-run-" + command.actionId;
        if (command.instruction.find(verifyProjectRequest) != std::string::npos) runId += verificationRunSuffix;
        return {thread, std::move(runId), ""};
    }

    Delivery send(const Command& command) override {
        std::lock_guard lock(mutex);
        if (command.tool == CommandTool::answer_session_question) {
            answered.insert(command.target.runId);
            return {Delivery::Kind::Accepted, command.target, ""};
        }
        auto delivery = reconcileLocked(command);
        // A request that starts with "ask:" makes the demo Session ask that question before it finishes.
        if (const auto at = command.instruction.find("ask:");
            at != std::string::npos && command.instruction.find("User answer:") == std::string::npos) {
            auto question = command.instruction.substr(at + 4, command.instruction.find('\n', at) - at - 4);
            if (question.find_first_not_of(" \t\r") == std::string::npos) question = "What should the demo focus on?";
            questions[delivery.run.runId] = question;
        }
        return delivery;
    }

    Delivery reconcile(const Command& command) override {
        std::lock_guard lock(mutex);
        return reconcileLocked(command);
    }

    Delivery reconcileLocked(const Command& command) { return {Delivery::Kind::Accepted, run(command), ""}; }

    Observation observe(const std::string&, const Json&, const RunRef& run) override {
        std::lock_guard lock(mutex);
        Observation observed;
        if (interrupted.contains(run.runId)) {
            observed.kind = Observation::Kind::Finished;
            observed.status = "cancelled";
            observed.resultRunId = run.runId;
            return observed;
        }
        if (const auto question = questions.find(run.runId); question != questions.end() && !answered.contains(run.runId)) {
            observed.kind = Observation::Kind::Question;
            observed.question = {"demo-question-" + run.runId, {{.id = "answer", .prompt = question->second}}};
            return observed;
        }
        observed.kind = Observation::Kind::Finished;
        observed.status = "completed";
        observed.report = run.runId.ends_with(verificationRunSuffix) ? verificationReport : "The demo finished its assigned work.";
        observed.resultRunId = run.runId;
        return observed;
    }

    std::vector<T3Project> projects(std::string_view environment) override {
        if (environment != "demo") return {};
        return {{"demo-project", "Demo project", {}}, {"demo-docs", "Demo documentation", {}}};
    }

    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef& run) override {
        std::lock_guard lock(mutex);
        interrupted.insert(run.runId);
        return InterruptOutcome::Requested;
    }

    std::mutex mutex;
    std::set<std::string> interrupted;
    std::map<std::string, std::string> questions;
    std::set<std::string> answered;
};
} // namespace

std::unique_ptr<Model> makeDemoModel() {
    return std::make_unique<DemoModel>();
}
std::unique_ptr<T3> makeDemoT3() {
    return std::make_unique<DemoT3>();
}
} // namespace flockv0::app
