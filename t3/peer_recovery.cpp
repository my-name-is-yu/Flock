#include "flock/access.hpp"
#include "internal.hpp"

#include <iostream>

using namespace flockv0;
using namespace flockv0::t3detail;

namespace {
void check(bool pass, std::string_view reason) {
    if (!pass) throw std::runtime_error(std::string(reason));
}

class PeerModel final : public Model {
public:
    int received = 0;

    Decision decide(const ModelInput& input) override {
        const auto& kind = input.event.at("kind");
        const auto& bot = input.task.at("botId");
        if (bot == "release" && kind == "intake")
            return {"send_bot_message",{{"botId","verifier"},{"kind","work"},
                     {"text","Verify the fixture"},{"doneWhen","The evidence is returned"}},"Use Verifier"};
        if (bot == "verifier" && kind == "bot_request")
            return {"delegate",{{"instruction","Verify the fixture"},{"doneWhen","T3 reports the evidence"}},
                    "Run the verifier"};
        if (bot == "verifier" && kind == "result") {
            check(input.event.at("run").at("threadId") == "thread" &&
                  input.event.at("resultRunId") == "root" && input.event.at("report") == "root report",
                  "Verifier received the wrong T3 result");
            return {"reply_to_bot",{{"kind","result"},{"text",input.event.at("report")}},"Return the original Run result"};
        }
        if (bot == "release" && kind == "bot_result") {
            const auto& refs = input.event.at("references");
            check(input.event.at("fromBotId") == "verifier" && input.event.at("text") == "root report" &&
                  refs.size() == 1 && refs.at(0).at("threadId") == "thread" && refs.at(0).at("runId") == "root",
                  "Release received the wrong result or evidence");
            ++received;
            return {"complete",{{"report",input.event.at("text")}},"The original Run report was delivered"};
        }
        throw Conflict("Unexpected model event");
    }
};

std::vector<std::string> eventTexts(const Json& conversation, std::string_view kind, std::string_view field) {
    std::vector<std::string> texts;
    for (const auto& event : conversation)
        if (event.at("kind") == kind) texts.push_back(event.at("data").at(field).get<std::string>());
    return texts;
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 5,"expected fixture URL, credential directory, database and recovery phase");
        const std::string origin = argv[1];
        const std::string_view phase = argv[4];
        Store store(argv[3]);
        if (phase == "start") {
            loginT3(argv[2],"env-1",origin + "/mcp",[&](const std::string& authorization) {
                const auto body = Json{{"authorization",authorization}}.dump();
                check(http(origin + "/fixture","POST",{{"Content-Type","application/json"}},body).status == 200,
                      "fixture rejected OAuth");
            });
            store.access().resourceGrant({"fixture",true},"verifier",{"environment","env-1","launch"},GrantChange::grant);
            store.submit({"release","Check the fixture","env-1","project"});
        } else check(phase == "recover","unknown recovery phase");

        auto adapter = makeT3Mcp(argv[2]);
        PeerModel model;
        RuntimeOptions options;
        options.peers = [](std::string_view bot) {
            if (bot == "release")
                return std::vector<PeerBot>{{"Verifier",{"verifier","","env-1","project"}}};
            return std::vector<PeerBot>{};
        };
        const Id source = store.tasks("release").at(0).at("id");
        const auto originalRecipients = store.tasks("verifier");
        if (phase == "recover") {
            check(store.task(source).at("state") == "peer_waiting" && originalRecipients.size() == 1,
                  "the original Bot exchange did not survive the crash");
            const auto& pending = originalRecipients.at(0);
            check(pending.at("state") == "sending" && pending.at("runs").empty(),
                  "the accepted launch must still be unconfirmed when recovery starts");
        }
        Runtime runtime(store,model,*adapter,{},options);
        for (int tick = 0; tick < 20; ++tick) runtime.tick();
        check(phase == "recover","T3 sent back a response before the Runtime was killed");

        const auto recipients = store.tasks("verifier");
        check(recipients.size() == 1 && originalRecipients.size() == 1 &&
              recipients.at(0).at("id") == originalRecipients.at(0).at("id"),"recovery created a different recipient task");
        const auto& verifier = recipients.at(0);
        check(verifier.at("state") == "completed","the recovered Verifier task did not finish");
        const auto& runs = verifier.at("runs");
        check(runs.size() == 1 && runs.at(0).at("threadId") == "thread" && runs.at(0).at("runId") == "root",
              "Verifier did not adopt the one original Run");
        check(eventTexts(store.conversation("verifier"),"report","report") == std::vector<std::string>{"root report"},
              "Verifier did not receive one report from the recovered Run");
        const auto releaseHistory = store.conversation("release");
        check(eventTexts(releaseHistory,"bot_message","text") ==
                  std::vector<std::string>{"Verify the fixture","root report"} &&
              eventTexts(releaseHistory,"completed","text") == std::vector<std::string>{"root report"} &&
              store.task(source).at("state") == "completed" && model.received == 1,
              "Release did not consume exactly one result");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
