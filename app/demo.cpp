#include "demo.hpp"
#include "flock/command.hpp"
#include "flock/bot_setup.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/model_error.hpp"
#include "flock/run_ref.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
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
constexpr std::string_view verificationReport = "The demo project's full check runs with ./scripts/verify.sh.";
constexpr std::string_view verificationRunSuffix = "-verify-project";

class DemoModel final : public Model {
    ModelTurn decide(const ModelInput& in) override {
        const auto event = in.event.value("kind", "");
        if (event == "conversation_turn") {
            const auto& turn = in.event.at("turn");
            if (!turn.empty()) {
                const auto& last = turn.back();
                const bool ok = last.at("result").value("ok", false);
                const auto tool = last.value("tool", "");
                std::string text = !ok                              ? "The demo could not apply that request."
                                   : tool == "write_memory"         ? "I’ll remember that."
                                   : tool == "answer_task_question" ? "I passed your answer to the task."
                                   : tool == "read_result"          ? "The demo result records successful completion of simulated work. "
                                                                      "It contains no implementation details."
                                                                    : "I queued your request.";
                return {{{"reply", {{"text", text}}, {}}}};
            }
            if (in.event.value("text", "") == setupRequest && std::ranges::find(in.tools, botSetupTool) != in.tools.end()) {
                auto role = in.notes.soul;
                if (!role.empty() && !role.ends_with('\n')) role += '\n';
                role += "Always run ./scripts/verify.sh before reporting.";
                return {{{std::string(botSetupTool), {{"name", "Release Scout"}, {"soul", std::move(role)}}, {}},
                         {"reply", {{"text", "I’m now Release Scout. I’ll run ./scripts/verify.sh before reporting."}}, {}}}};
            }
            if (in.event.at("text") == fridayRequest && std::ranges::find(in.tools, "write_memory") != in.tools.end()) {
                auto content = in.notes.memory;
                if (content.find(fridayMemory) == content.npos) {
                    if (!content.empty() && !content.ends_with('\n')) content += '\n';
                    content.append(fridayMemory);
                }
                return {{{"write_memory",
                          {{"baseVersion", in.event.at("memoryVersion")}, {"content", content}, {"summary", fridayMemory}},
                          {}}}};
            }
            const auto& reply = in.event.at("replyTo");
            if (reply.is_object() && reply.value("kind", "") == "task_question" && reply.value("current", false)) {
                return {{{"answer_task_question",
                          {{"taskId", reply["taskId"]},
                           {"waitId", reply["waitId"]},
                           {"revisionId", reply["revisionId"]},
                           {"generation", reply["generation"]},
                           {"text", in.event.value("text", "")}},
                          {}}}};
            }
            if (reply.is_object() && reply.value("kind", "") == "result")
                return {{{"read_result", {{"messageId", reply["messageId"]}}, {}}}};
            if (reply.is_object() && reply.value("kind", "") == "bot_message") {
                const auto source = reply.value("inReplyTo", Json::object()).value("replyToMessageId", Json(nullptr));
                if (!source.is_null()) return {{{"read_result", {{"messageId", source}}, {}}}};
            }
            return {{{"start_task", {{"request", in.event.value("text", "")}}, {}}}};
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

    Delivery send(const Command& command) override { return reconcile(command); }

    Delivery reconcile(const Command& command) override { return {Delivery::Kind::Accepted, run(command), ""}; }

    Observation observe(const std::string&, const Json&, const RunRef& run) override {
        Observation observed;
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

    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { return InterruptOutcome::Requested; }
};
} // namespace

std::unique_ptr<Model> makeDemoModel() {
    return std::make_unique<DemoModel>();
}
std::unique_ptr<T3> makeDemoT3() {
    return std::make_unique<DemoT3>();
}
} // namespace flockv0::app
