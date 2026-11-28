#pragma once

#include "flock/task_control.hpp"
#include "flock/task_target.hpp"
#include "flock/task_history.hpp"
#include "flock/model_error.hpp"
#include "flock/person.hpp"
#include "flock/task_intake.hpp"
#include "flock/tasks.hpp"
#include "flock/decision.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/runtime.hpp"
#include "flock/runtime_options.hpp"
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

// Tasks never call a model, so any call is a failure of the fixture.
class TestModel final : public Model {
public:
    unsigned calls = 0;
    ModelTurn decide(const ModelInput&) override {
        ++calls;
        throw ModelError("The live T3 fixture does not expect model calls", false);
    }
};

inline std::string caseInstruction(std::string_view request) {
    if (request == "smoke")
        return std::string("Reply with '") + smokeReport + "' without quotes or punctuation. Do not use tools or edit files.";
    if (request == "interrupt") return "Start the harmless terminal command sleep 60 and wait for it to finish. Do not edit any files.";
    if (request == "question")
        return "Use the available user-input tool to ask which of main or dev is the target branch. "
               "Wait for that answer in the same Run. Then report the chosen branch. Do not edit any files.";
    if (request == "ambiguous-question") return sameQuestion;
    if (request == "approval")
        return "Use the terminal tool to execute `printf 'approval-fixture\\n'`, with "
               "sandbox_permissions=require_escalated and justification 'Verify the Flock approval hold'. "
               "Wait for approval if one is requested. Do not edit files.";
    if (request == "delegated-result")
        return "Start an asynchronous T3 delegate using the delegate_task tool, asking it to read notes.md "
               "and identify the two pending owners and their items. Do not write files. "
               "After launching it, end this turn with 'child started'. When T3 wakes you with completion, "
               "write the delegate's final list. Do not launch a second delegate.";
    if (request.starts_with("measurement-"))
        return std::string("Execute exactly the shell command `./measure_") + (request == "measurement-mismatch" ? "off" : "on") +
               ".sh`. Do not edit any project file. "
               "Leave the measurement output in the terminal activity. The entire final answer must be exactly Measurement complete, with no punctuation or details.";
    if (request == "profile-short" || request == "profile-long")
        return std::string(request == "profile-short" ? "Run `printf 'short-history-fixture\\n'` in a terminal."
                                                      : "Run `seq 1 25000` in a terminal and request max_output_tokens=16000.") +
               " Then execute `sleep 45` in a separate terminal tool call and wait for it to finish. Do not edit files.";
    return "Read notes.md in this fixture project. Report the two pending items with their owners. Keep the repository unchanged.";
}

inline constexpr std::string_view t3Scenarios[] = {"smoke",
                                                   "summary",
                                                   "interrupt",
                                                   "question",
                                                   "ambiguous-question",
                                                   "approval",
                                                   "delegated-result",
                                                   "measurement-match",
                                                   "measurement-mismatch",
                                                   "profile-short",
                                                   "profile-long"};

inline Id submit(Store& store, std::string_view environmentId, std::string_view projectId, std::string_view scenario,
                 std::string_view providerId = {}, std::string_view model = {}) {
    if (std::ranges::find(t3Scenarios, scenario) == std::ranges::end(t3Scenarios))
        throw Conflict("Unknown T3 fixture case: " + std::string(scenario));
    Json config = {{"session", {{"runtimeMode", "auto"}}}};
    if (!providerId.empty() || !model.empty()) {
        config["session"]["providerInstanceId"] = providerId;
        config["session"]["model"] = model;
    }
    if (scenario == "question" || scenario == "ambiguous-question") config["session"]["interactionMode"] = "plan";
    if (const auto* mode = std::getenv("FLOCK_ACCEPTANCE_RUNTIME_MODE")) config["session"]["runtimeMode"] = mode;
    return TaskIntake(store).submit({"acceptance", caseInstruction(scenario), std::string(environmentId), std::string(projectId), config});
}

inline int pump(Store& store, T3& t3, Id id, std::string_view targetState) {
    using namespace std::chrono_literals;
    TestModel model;
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string("Validate the acceptance fixture"); });
    std::string previous;
    const auto end = std::chrono::steady_clock::now() + 5min;
    do {
        const auto task = taskJson(Tasks(store).task(id));
        const auto state = task.at("state").get<std::string>();
        if (previous != state) {
            std::cout << state << " ";
            if (!task.at("activeRun").is_null()) std::cout << task.at("activeRun").dump();
            if (!task.at("wait").is_null()) std::cout << task.at("wait").dump();
            std::cout << std::endl;
            previous = state;
        }
        if (state == targetState) return 0;
        const auto reason = state == "waiting" ? task.at("wait").value("reason", "") : std::string{};
        if (reason == "user_question") {
            // The fixture's person always targets main.
            TaskControl(store).operate(localOwner, TaskTarget::from(Tasks(store).task(id).task, task.at("wait").at("id")), "answer",
                                       {{"text", "main"}});
        } else if ((state == "waiting" && reason != "t3_input") || state == "completed" || state == "cancelled")
            return 3;
        runtime.tick();
        std::this_thread::sleep_for(250ms);
    } while (std::chrono::steady_clock::now() < end);
    std::cerr << "Live fixture timed out waiting for " << targetState << '\n';
    return 4;
}
}
