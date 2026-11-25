#pragma once

#include "flock/decision.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/runtime.hpp"
#include "flock/store.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace flockv0::live_test {
inline constexpr auto sameQuestion =
    "Use the user-input tool to ask exactly one question with id target_branch, header Branch, "
    "question 'Which branch?', options main with description 'Use main.' and dev with description 'Use dev.'. "
    "Do not rephrase any fields. Wait for the answer and then report the chosen branch. Do not edit files.";

inline constexpr auto smokeReport = "Flock live T3 smoke complete";

class TestModel final : public Model {
public:
    unsigned calls = 0;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        ++calls;
        const auto request = input.task.at("request").get<std::string>();
        if (input.event.at("kind") == "intake") {
            std::string instruction =
                "Read notes.md in this fixture project. Report the two pending items with their owners. Keep the repository unchanged.";
            if (request == "smoke")
                instruction =
                    std::string("Reply with '") + smokeReport + "' without quotes or punctuation. Do not use tools or edit files.";
            if (request == "interrupt") {
                instruction = "Start the harmless terminal command sleep 60 and wait for it to finish. Do not edit any files.";
            }
            if (request == "question") {
                instruction = "Use the available user-input tool to ask which of main or dev is the target branch. "
                              "Wait for that answer in the same Run. Then report the chosen branch. Do not edit any files.";
            }
            if (request == "ambiguous-question") instruction = sameQuestion;
            if (request == "approval") {
                instruction = "Use the terminal tool to execute `printf 'approval-fixture\\n'`, with "
                              "sandbox_permissions=require_escalated and justification 'Verify the Flock approval hold'. "
                              "Wait for approval if one is requested. Do not edit files.";
            }
            if (request == "delegated-result") {
                instruction = "Start an asynchronous T3 delegate using the delegate_task tool, asking it to read notes.md "
                              "and identify the two pending owners and their items. Do not write files. "
                              "After launching it, end this turn with 'child started'. When T3 wakes you with completion, "
                              "write the delegate's final list. Do not launch a second delegate.";
            }
            if (request.starts_with("measurement-")) {
                const auto flags = request == "measurement-mismatch" ? "off" : "on";
                instruction =
                    std::string("Execute exactly the shell command `./measure_") + flags +
                    ".sh`. Do not edit any project file. "
                    "Leave the measurement output in the terminal activity. The entire final answer must be exactly Measurement complete, with no punctuation or details.";
            }
            if (request == "profile-short" || request == "profile-long") {
                instruction = request == "profile-short" ? "Run `printf 'short-history-fixture\\n'` in a terminal."
                                                         : "Run `seq 1 25000` in a terminal and request max_output_tokens=16000.";
                instruction += " Then execute `sleep 45` in a separate terminal tool call and wait for it to finish. Do not edit files.";
            }
            return {"delegate",
                    {{"instruction", instruction}, {"doneWhen", "The fixture task and its result are documented"}},
                    "Send the fixed acceptance case to the isolated fixture project."};
        }
        if (input.event.at("kind") == "question") {
            auto answers = Json::object();
            const auto& question = input.event.at("question");
            for (const auto& item : question.at("fields")) {
                auto& answer = answers[item.at("id").get<std::string>()];
                answer = "main";
                for (const auto& option : item.value("options", Json::array())) {
                    if (option.is_object() && option.value("value", std::string{}).starts_with("main")) {
                        answer = option["value"];
                        break;
                    }
                }
            }
            return {
                "answer_session_question", {{"requestId", question.at("requestId")}, {"answers", answers}}, "The fixture targets main."};
        }
        const auto report = input.event.at("report").get<std::string>();
        if (input.event.at("status") != "completed" || report.empty())
            return {"ask_user",
                    {{"question", "The fixture Run did not produce a successful report. Check T3."}},
                    "The Run status and report do not provide the required evidence."};
        if (request == "summary-follow" && input.task.at("additionalUsed") == 0)
            return {"follow_up",
                    {{"instruction",
                      "Check that you named both owners from notes.md, and repeat the two pending items. Do not modify any files."}},
                    "Confirm the complete list within the same Thread."};
        if (request == "measurement-read-resume") {
            if (input.evidence.empty()) {
                return {
                    "session_read",
                    {{"threadId", input.event.at("run").at("threadId")}, {"runId", input.event.at("resultRunId")}, {"view", "activity"}},
                    "Read the live measurement activity."};
            }
            if (input.evidence.size() != 1 || input.evidence[0].at("text").get<std::string>().empty())
                throw Conflict("The saved live excerpt was not restored");
        }
        if (request == "ambiguous-question" && input.task.at("additionalUsed") == 0)
            return {"follow_up", {{"instruction", sameQuestion}}, "Repeat the exact same question on a separate Run."};
        if ((request.starts_with("summary") || request == "delegated-result") &&
            (report.find("Mina") == std::string::npos || report.find("Alex") == std::string::npos))
            return {
                "ask_user", {{"question", "The report omitted an owner. Check the fixture output."}}, "The fixture requires both names."};
        return {"complete", {{"report", report}}, "The report from the assigned T3 Run meets the fixture criteria."};
    }
};

inline constexpr std::string_view t3Scenarios[] = {"smoke",
                                                   "summary",
                                                   "summary-follow",
                                                   "interrupt",
                                                   "question",
                                                   "ambiguous-question",
                                                   "approval",
                                                   "delegated-result",
                                                   "measurement-match",
                                                   "measurement-mismatch",
                                                   "measurement-read-resume",
                                                   "profile-short",
                                                   "profile-long"};

inline Id submit(Store& store, std::string_view environmentId, std::string_view projectId, std::string_view scenario,
                 std::string_view providerId = {}, std::string_view model = {}) {
    if (std::ranges::find(t3Scenarios, scenario) == std::ranges::end(t3Scenarios))
        throw Conflict("Unknown T3 fixture case: " + std::string(scenario));
    Json config = {{"runTimeoutSeconds", 300}, {"session", {{"runtimeMode", "auto"}}}};
    if (!providerId.empty() || !model.empty()) {
        config["session"]["providerInstanceId"] = providerId;
        config["session"]["model"] = model;
    }
    if (scenario == "question" || scenario == "ambiguous-question") config["session"]["interactionMode"] = "plan";
    if (const auto* mode = std::getenv("FLOCK_ACCEPTANCE_RUNTIME_MODE")) config["session"]["runtimeMode"] = mode;
    return store.submit({"acceptance", std::string(scenario), std::string(environmentId), std::string(projectId), config});
}

inline int pump(Store& store, T3& t3, Id id, std::string_view targetState) {
    using namespace std::chrono_literals;
    TestModel model;
    RuntimeOptions options;
    if (store.task(id).task.request == "measurement-read-resume") options.sessionRead = "on_demand";
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string("Validate the acceptance fixture"); }, options);
    std::string previous;
    const auto end = std::chrono::steady_clock::now() + 5min;
    do {
        const auto task = taskJson(store.task(id));
        const auto state = task.at("state").get<std::string>();
        if (previous != state) {
            std::cout << state << " ";
            if (!task.at("activeRun").is_null()) std::cout << task.at("activeRun").dump();
            if (!task.at("wait").is_null()) std::cout << task.at("wait").dump();
            std::cout << std::endl;
            previous = state;
        }
        if (state == targetState) return 0;
        if (targetState == "read-saved") {
            const auto history = store.history(id);
            if (std::any_of(history.begin(), history.end(), [](const Json& item) { return item["kind"] == "session_read"; })) return 0;
        }
        if ((state == "waiting" && task.at("wait").value("reason", "") != "t3_input") || state == "completed" || state == "cancelled")
            return 3;
        runtime.tick();
        std::this_thread::sleep_for(250ms);
    } while (std::chrono::steady_clock::now() < end);
    std::cerr << "Live fixture timed out waiting for " << targetState << '\n';
    return 4;
}
}
