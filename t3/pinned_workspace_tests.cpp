#include "internal.hpp"
#include <iostream>

using namespace flockv0;
using flockv0::t3detail::http;

namespace {
void check(bool passed, std::string_view reason) {
    if (!passed) throw std::logic_error(std::string(reason));
}

}

int main(int argc, char** argv) {
    try {
        check(argc == 3,"expected fixture URL and credential directory");
        const std::string origin = argv[1];
        const std::filesystem::path directory = argv[2];
        auto fixture = [&](const Json& settings) {
            check(http(origin+"/fixture","POST",{{"Content-Type","application/json"}},settings.dump()).status == 200,
                  "fixture rejected configuration");
        };
        const auto commits = Json::parse(http(origin+"/fixture/stock","GET").body);
        const auto pin = commits.at("stockPin").get<std::string>();
        loginT3(directory,"env-1",origin+"/mcp",[&](const auto& auth) { fixture({{"authorization",auth}}); });
        const std::string environment = "env-1";
        auto adapter = makeT3Mcp(directory);
        const auto generation = adapter->authGeneration(environment,Json::object());
        Command command;
        command.actionId = "pinned-worker";
        command.tool = "delegate";
        command.environmentId = environment;
        command.projectId = "project";
        command.authGeneration = generation;
        command.instruction = "Check fixture";
        command.config["session"] = {{"runtimeMode","approval-required"},
            {"workspaceStrategy",{{"type","worktree"},{"baseRef",pin},{"startFromOrigin",false},{"expectedCommit",pin}}}};
        check(!adapter->preflight(command), "valid pinned specialist preflight");
        for (const auto mode : {"unstaged","staged"}) {
            fixture({{"stockTrackedEdit",mode}});
            command.actionId = std::string("dirty-") + mode;
            const auto rejected = adapter->send(command);
            check(rejected.kind == Delivery::Kind::NotAccepted && rejected.definitelyUnsent,
                  "tracked " + std::string(mode) + " edit blocks the first specialist prompt");
            fixture({{"stockTrackedEdit","clean"}});
        }
        command.actionId = "pinned-worker";
        t3detail::Client provision(t3detail::loadCredential(directory,environment));
        const auto provisioned = provision.call("t3_thread_launch",{{"projectId",command.projectId},
            {"title","Flock task"},{"clientRequestId",command.actionId},{"runtimeMode","approval-required"},
            {"workspaceStrategy",{{"type","worktree"},{"baseRef",pin},{"startFromOrigin",false}}}});
        check(provisioned.at("threadId")==command.actionId && provisioned.at("runId").is_null(),
              "fixture did not leave a provision-only launch for same-key recovery");
        const auto started = adapter->send(command);
        check(started.kind == Delivery::Kind::Accepted && started.run.threadId == command.actionId &&
              started.run.runId == "next-run", "idle stock workspace starts only after its input pin matches");
        for (const auto mode : {"unstaged","staged"}) {
            fixture({{"stockTrackedEdit",mode}});
            const auto dirty = adapter->observe(environment,command.config,started.run);
            check(dirty.kind == Observation::Kind::Finished && dirty.status == "failed" && !dirty.report.empty(),
                  "tracked " + std::string(mode) + " edit did not produce terminal failed evidence");
            fixture({{"stockTrackedEdit","clean"}});
        }
        const auto report = adapter->observe(environment,command.config,started.run);
        check(report.kind == Observation::Kind::Finished && report.status == "completed" && report.report == "Pinned report",
              "provider report from the pinned workspace is accepted");
        fixture({{"advanceStockWorker",true}});
        const auto changed = adapter->observe(environment,command.config,started.run);
        check(changed.kind == Observation::Kind::Finished && changed.status == "failed" && changed.report != "Pinned report",
              "settled specialist result is invalidated when the actual workspace drifted");
        command.actionId = "wrong-worker";
        const auto wrong = adapter->send(command);
        check(wrong.kind == Delivery::Kind::NotAccepted && wrong.definitelyUnsent,
              "different actual worktree input blocks the first prompt");
        const auto effects = Json::parse(http(origin+"/fixture/effects","GET").body);
        const auto stock = Json::parse(http(origin+"/fixture/stock","GET").body);
        check(effects.at("t3_thread_send").size() == 1 && effects.at("t3_thread_launch").size() == 5 &&
              !effects.contains("t3_thread_interrupt") && stock.at("stockLaunches").size()==4 &&
              stock.at("stockLaunches").at("project:pinned-worker").at("replays")==1 &&
              stock.at("stockPromptSends").at("pinned-worker:pinned-worker:start").at("replays")==0,
              "same-key provision recovery created another thread, resent the first prompt, or sent to invalid work");

        std::cout << "Pinned T3 workspace verification passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
