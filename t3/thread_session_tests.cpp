#include "internal.hpp"
#include "flock/session_handoff.hpp"

#include <algorithm>
#include <iostream>

using namespace flockv0;
using flockv0::t3detail::http;

namespace {
void check(bool passed, std::string_view reason) {
    if (!passed) throw std::logic_error(std::string(reason));
}

class ReplyBot final : public Model {
public:
    ModelTurn decide(const ModelInput&) override {
        ++calls;
        return {{{"reply",{{"text","Fixture reply"}},""}}};
    }
    int calls = 0;
};
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
        auto session = makeT3ThreadSession(directory);
        const ThreadTarget target{"env-1","project","human"};
        const auto page = session->read(target,{});
        check(page && page->cursor == 4 && page->events.size() == 1 &&
              page->events.front().creationSource == "web" && page->events.front().createdBy == "user",
              "newest page excludes inherited and unfinalized messages and holds the cursor");
        fixture({{"finalized",true}});
        const auto later = session->read(target,page->cursor);
        check(later && !later->hasMore && later->cursor == 6 && later->events.size() == 2 &&
              later->events.back().messageId == "sent-id" &&
              later->events.back().creationSource == "mcp" && later->events.back().createdBy == "agent",
              "forward page includes finalized progress and correlated MCP delivery");

        fixture({{"stockHistory","late-human"}});
        const auto firstPage = session->read(target,0);
        const auto fullHistory = session->dispatchHistory(target,0);
        check(firstPage && firstPage->hasMore && firstPage->cursor == 20 && fullHistory && fullHistory->cursor == 26 &&
              std::ranges::any_of(fullHistory->events,[](const auto& event) {
                  return event.position == 26 && event.kind == "user_message" && event.createdBy == "user";
              }),"dispatch history did not paginate beyond the finalized observation page");
        fixture({{"stockHistory","streaming-late-human"}});
        const auto streamingPage = session->read(target,0);
        const auto streamingHistory = session->dispatchHistory(target,0);
        check(streamingPage && streamingPage->hasMore && streamingPage->cursor == 0 &&
              streamingPage->events.empty() && streamingHistory &&
              streamingHistory->events.size() == 2 && streamingHistory->events.back().createdBy == "user",
              "dispatch history stopped at an unfinished assistant message");
        fixture({{"stockIncomplete",true}});
        check(!session->dispatchHistory(target,0),"an incomplete bounded history scan was treated as complete");
        fixture({{"stockHistory","default"},{"stockIncomplete",false}});

        const auto generation = page->authGeneration;
        const auto send = [&](const ThreadTarget& destination, std::string_view auth = "") {
            return session->send(destination,auth.empty() ? generation : auth,"command-1","[Flock result] Complete.");
        };
        check(send(target,"old-generation").kind == ThreadSend::Rejected, "stale OAuth namespace refuses the send");
        fixture({{"claimedProject","wrong"}});
        check(!session->read(target,{}) && send(target).kind == ThreadSend::Rejected,
              "mismatched project fails closed");
        fixture({{"claimedProject","project"},{"claimedEnvironment","wrong"}});
        check(!session->read(target,{}) && send(target).kind == ThreadSend::Rejected,
              "mismatched environment fails closed");
        fixture({{"claimedEnvironment","env-1"}});
        const auto delivery = send(target);
        check(delivery.kind == ThreadSend::Accepted && delivery.receipt && delivery.receipt->mode == "queued" &&
              delivery.receipt->messageId == "sent-id" && delivery.receipt->runId == "next-run",
              "queue receipt can identify a later Run in the same Thread");
        fixture({{"badReceipt",true}});
        check(send(target).kind == ThreadSend::Unknown, "mismatched post-dispatch receipt stays unknown");
        fixture({{"badReceipt",false}});

        check(session->inputCommit(target) == commits.at("stockHead"), "input pin comes from the bound workspace");
        auto adapter = makeT3Mcp(directory);
        Command command;
        command.actionId = "pinned-worker";
        command.tool = "delegate";
        command.environmentId = target.environmentId;
        command.projectId = target.projectId;
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
        t3detail::Client provision(t3detail::loadCredential(directory,target.environmentId));
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
            const auto dirty = adapter->observe(target.environmentId,command.config,started.run);
            check(dirty.kind == Observation::Kind::Finished && dirty.status == "failed" && !dirty.report.empty(),
                  "tracked " + std::string(mode) + " edit did not produce terminal failed evidence");
            fixture({{"stockTrackedEdit","clean"}});
        }
        const auto report = adapter->observe(target.environmentId,command.config,started.run);
        check(report.kind == Observation::Kind::Finished && report.status == "completed" && report.report == "Pinned report",
              "provider report from the pinned workspace is accepted");
        fixture({{"advanceStockWorker",true}});
        const auto changed = adapter->observe(target.environmentId,command.config,started.run);
        check(changed.kind == Observation::Kind::Finished && changed.status == "failed" && changed.report != "Pinned report",
              "settled specialist result is invalidated when the actual workspace drifted");
        command.actionId = "wrong-worker";
        const auto wrong = adapter->send(command);
        check(wrong.kind == Delivery::Kind::NotAccepted && wrong.definitelyUnsent,
              "different actual worktree input blocks the first prompt");
        const auto effects = Json::parse(http(origin+"/fixture/effects","GET").body);
        const auto stock = Json::parse(http(origin+"/fixture/stock","GET").body);
        check(effects.at("t3_thread_send").size() == 3 && effects.at("t3_thread_launch").size() == 5 &&
              !effects.contains("t3_thread_interrupt") && stock.at("stockLaunches").size()==4 &&
              stock.at("stockLaunches").at("project:pinned-worker").at("replays")==1 &&
              stock.at("stockPromptSends").at("pinned-worker:pinned-worker:start").at("replays")==0,
              "same-key provision recovery created another thread, resent the first prompt, or sent to invalid work");

        fixture({{"stockRootRecovery",true},{"stockDropSendReply",true}});
        const auto database = directory.parent_path() / "root-recovery.sqlite3";
        const Person owner{"owner",true};
        const auto people = [&](std::string_view person) -> std::optional<Person> {
            return person == owner.id ? std::optional(owner) : std::nullopt;
        };
        const auto intake = [&](std::string_view bot, std::string_view) {
            return Intake{std::string(bot),"",target.environmentId,target.projectId};
        };
        const auto soul = [](std::string_view) { return std::string{}; };
        ReplyBot bot;
        Id channel;
        {
            Store store(database);
            store.access().resourceGrant(owner,"planner",Scope::consultReply(target.environmentId,target.threadId),
                                         GrantChange::grant);
            SessionHandoff handoff(store,*session,people);
            const auto pair = handoff.pair(owner,target,"planner",{});
            channel = handoff.intake(pair.code,"failed-root-run","Check the fixture.").channel;
            handoff.consult(owner,channel,bot,intake,soul);
            check(handoff.view(owner,channel).deliveries.front().state == RootDeliveryState::Unknown,
                  "a dropped send response was reported as confirmed");
        }
        fixture({{"stockDropSendReply",false},{"stockRootWait","wrong-run"}});
        auto reconnected = makeT3ThreadSession(directory);
        Store reopened(database);
        SessionHandoff recovered(reopened,*reconnected,people);
        recovered.recover();
        recovered.consult(owner,channel,bot,intake,soul);
        check(recovered.view(owner,channel).deliveries.front().observedRunId == "next-run",
              "history did not identify the run after the lost reply");
        for (const auto wait : {"wrong-thread","wrong-run","timed-out","cancelled"}) {
            fixture({{"stockRootWait",wait}});
            recovered.consult(owner,channel,bot,intake,soul);
            const auto pending = recovered.view(owner,channel);
            check(pending.state == "active" && pending.deliveries.front().state == RootDeliveryState::Unknown,
                  "unproven failure settled the delivery");
        }
        fixture({{"stockRootWait","failed"},{"stockRootExtraHistory",true}});
        recovered.consult(owner,channel,bot,intake,soul);
        check(recovered.view(owner,channel).deliveries.front().state == RootDeliveryState::Unknown,
              "run failure settled delivery before checking all available messages");
        recovered.consult(owner,channel,bot,intake,soul);
        const auto settled = recovered.view(owner,channel);
        check(settled.state == "failed" && settled.deliveries.front().state == RootDeliveryState::Failed,
              "confirmed root run failure without an assistant message left delivery unresolved");
        const auto recoveredEffects = Json::parse(http(origin+"/fixture/effects","GET").body);
        check(bot.calls == 1 && recoveredEffects.at("t3_thread_send").size() == 4,
              "failure reconciliation repeated the model or root send");
        std::cout << "Stock Thread delivery, lost reply recovery, and pinned specialist verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
