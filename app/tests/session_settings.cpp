#include "fixtures.hpp"
#include "../../t3/internal.hpp"
#include "flock/policy.hpp"

#include <iostream>

using namespace flockv0;
using namespace flockv0::app;
using namespace flockv0::app::test;
using flockv0::t3detail::http;

namespace {
class LaunchModel final : public Model {
    ModelTurn decide(const ModelInput&) override {
        return {{{"delegate",{{"instruction","Verify the fixture"},{"doneWhen","T3 reports the evidence"}},"Run the verifier"}}};
    }
};

void checkSessionSettings(const std::string& origin, const std::filesystem::path& credentials) {
    loginT3(credentials,"env-1",origin + "/mcp",[&](const std::string& authorization) {
        const auto body = Json{{"authorization",authorization},{"scenario","running"}}.dump();
        check(http(origin + "/fixture","POST",{{"Content-Type","application/json"}},body).status == 200,
              "fixture rejected OAuth");
    });
    const auto effects = [&] {
        const auto response = http(origin + "/fixture/effects","GET");
        check(response.status == 200,"could not read fixture effects");
        return Json::parse(response.body);
    };
    Sandbox temp;
    const auto folder = temp.path / "bots";
    const auto database = temp.path / "tasks.db";
    const Bots bots(folder);
    const auto intake = [&](std::string_view bot, std::string_view request) { return bots.get(bot).intake(request); };
    auto config = botConfig();
    config["environmentId"] = "env-1";
    config["projectId"] = "project";
    config["session"]["workspaceStrategy"]["branch"] = "fixture-work";

    const Json legacy = {{"workspaceStrategy",{{"kind","worktree"},{"remote","origin"},{"branch","main"}}}};
    const Json malformed = Json::array({
        nullptr, Json::array(), legacy,
        {{"model","child-model"}},
        {{"model"," \t "},{"providerInstanceId","child-provider"}},
        {{"runtimeMode","invalid"}},
        {{"runtimeMode",true}},
        {{"workspaceStrategy",{{"type","worktree"}}}},
        {{"workspaceStrategy",{{"type","existing_worktree"},{"worktreePath"," "}}}},
        {{"workspaceStrategy",{{"type","worktree"},{"baseRef","origin/main"},{"startFromOrigin","true"}}}},
        {{"interactionMode","plan"}}
    });
    for (const auto& session : malformed) {
        auto invalid = config;
        invalid["session"] = session;
        createBot(folder / "release",invalid,"","");
        rejects<Conflict>([&] { intake("release","Verify the fixture"); },"Bot intake accepted " + session.dump());
    }

    const Person owner{"fixture",true};
    Id id;
    {
        Store store(database);
        auto saved = config;
        saved["session"] = legacy;
        id = store.submit({"release","Verify the fixture","env-1","project",saved});
    }
    Store store(database);
    auto adapter = makeT3Mcp(credentials);
    LaunchModel model;
    Runtime runtime(store,model,*adapter,{});
    for (int i = 0; i < 3; ++i) runtime.tick();
    const auto waiting = store.task(id);
    check(waiting.at("state") == "waiting" && waiting.at("wait").at("reason") == "settings",
          "legacy settings did not leave an actionable settings wait");
    check(effects().empty(),"invalid persisted settings reached T3");

    const Id waitId = waiting.at("wait").at("id");
    const auto target = TaskTarget::from(waiting,waitId);
    for (const auto& session : malformed) {
        rejects<Conflict>([&] { store.operate(owner,target,"reconcile",{{"session",session}}); },
                          "settings correction accepted " + session.dump());
    }
    check(store.task(id).at("wait") == waiting.at("wait"),"invalid corrections cleared the settings wait");

    createBot(folder / "release",config,"","");
    store.operate(owner,target,"reconcile",reconciliationData(waiting,waitId,Json::object(),intake));
    for (int i = 0; i < 2; ++i) runtime.tick();
    const auto resumed = store.task(id);
    check(resumed.at("state") == "running","corrected task state was " + resumed.at("state").dump());
    const auto observed = effects();
    const auto& launches = observed.at("t3_thread_launch");
    check(launches.size() == 1,"correction should launch exactly one Session");
    const auto& args = launches.at(0);
    check(args.at("modelSelection") == Json{{"instanceId","child-provider"},{"model","child-model"}} &&
          args.at("runtimeMode") == "auto" &&
          args.at("workspaceStrategy") == Json{{"type","worktree"},{"baseRef","origin/main"},
                                               {"startFromOrigin",true},{"branch","fixture-work"}} &&
          !args.contains("model") && !args.contains("providerInstanceId") && !args.contains("interactionMode"),
          "T3 did not receive the configured Session overrides");

    Command direct;
    direct.tool = "delegate";
    direct.actionId = "fixture-direct-launch";
    direct.environmentId = "env-1";
    direct.projectId = "project";
    direct.instruction = "Verify the fixture in plan mode";
    direct.authGeneration = adapter->authGeneration(direct.environmentId,direct.config);
    direct.config["session"] = legacy;
    const auto invalidDelivery = adapter->send(direct);
    check(invalidDelivery.kind == Delivery::Kind::Settings && invalidDelivery.definitelyUnsent &&
          effects().at("t3_thread_launch").size() == 1,
          "direct T3 launch did not reject malformed settings before send");
    direct.config["session"] = {{"interactionMode","plan"},
                                {"workspaceStrategy",{{"type","existing_worktree"},{"worktreePath","/fixture/worktree"}}}};
    check(adapter->send(direct).kind == Delivery::Kind::Accepted,"direct T3 launch was rejected");
    const auto directArgs = effects().at("t3_thread_launch").at(1);
    check(directArgs.at("interactionMode") == "plan" &&
          directArgs.at("workspaceStrategy") == Json{{"type","existing_worktree"},{"worktreePath","/fixture/worktree"}},
          "direct T3 launch lost its interaction mode or worktree path");
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3,"expected fixture URL and credential directory");
        checkSessionSettings(argv[1],argv[2]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
