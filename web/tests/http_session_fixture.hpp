#pragma once

#include "http_fixture.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"

#include <map>
#include <vector>

namespace flockv0::http_test {
struct FixedT3 : T3 {
    std::map<std::string, Observation, std::less<>> observations;
    std::vector<Command> replies;
    std::map<std::string, unsigned, std::less<>> launches;

    std::string authGeneration(const std::string&, const Json&) override { return "fixture-auth"; }
    Delivery send(const Command& command) override {
        if (command.tool == CommandTool::delegate) {
            if (command.projectId == "settings" &&
                command.config.value("session", Json::object()).value("runtimeMode", "") != "approval-required")
                return {Delivery::Kind::Settings, {}, privateValue, true};
            const auto& project = command.projectId;
            const auto link = project == "triage" ? "http://localhost:3773/linked%20env/thread-triage" : "javascript:unexpected";
            return {
                Delivery::Kind::Accepted, {"thread-" + project, "run-" + project + "-" + std::to_string(++launches[project]), link}, {}};
        }
        check(command.tool == CommandTool::answer_session_question, "unexpected command in fixture");
        replies.push_back(command);
        observations.erase(command.target.threadId);
        return {Delivery::Kind::Accepted, {}, {}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef& run) override { return observations[run.threadId]; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { return InterruptOutcome::Requested; }
};
}
