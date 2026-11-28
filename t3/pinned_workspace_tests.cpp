#include "internal.hpp"
#include "../tests/support.hpp"
#include "flock/command.hpp"
#include "flock/json.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/t3_mcp.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace flockv0;
using flockv0::t3detail::http;

namespace {
using flockv0::test::check;

}

int main(int argc, char** argv) {
    try {
        check(argc == 3, "expected fixture URL and credential directory");
        const std::string origin = argv[1];
        const std::filesystem::path directory = argv[2];
        auto fixture = [&](const Json& settings) {
            check(http(origin + "/fixture", "POST", {{"Content-Type", "application/json"}}, settings.dump()).status == 200,
                  "fixture rejected configuration");
        };
        const auto commits = Json::parse(http(origin + "/fixture/stock", "GET").body);
        const auto pin = commits.at("stockPin").get<std::string>();
        loginT3(directory, "env-1", origin + "/mcp", [&](const auto& auth) { fixture({{"authorization", auth}}); });
        const std::string environment = "env-1";
        auto adapter = makeT3Mcp(directory);
        const auto generation = adapter->authGeneration(environment, Json::object());
        Command command;
        command.actionId = "pinned-worker";
        command.tool = CommandTool::delegate;
        command.environmentId = environment;
        command.projectId = "project";
        command.authGeneration = generation;
        command.instruction = "Check fixture";
        command.config["session"] = {
            {"runtimeMode", "approval-required"},
            {"workspaceStrategy", {{"type", "worktree"}, {"baseRef", pin}, {"startFromOrigin", false}, {"expectedCommit", pin}}}};
        check(!adapter->preflight(command), "valid pinned specialist preflight");
        for (const auto mode : {"unstaged", "staged"}) {
            fixture({{"stockTrackedEdit", mode}});
            command.actionId = std::string("dirty-") + mode;
            const auto rejected = adapter->send(command);
            check(rejected.kind == Delivery::Kind::NotAccepted && rejected.definitelyUnsent,
                  "tracked " + std::string(mode) + " edit blocks the first specialist prompt");
            fixture({{"stockTrackedEdit", "clean"}});
        }
        command.actionId = "pinned-worker";
        t3detail::Client provision(t3detail::loadCredential(directory, environment));
        const auto provisioned = provision.call(
            "t3_thread_launch", {{"projectId", command.projectId},
                                 {"title", "Flock task"},
                                 {"clientRequestId", command.actionId},
                                 {"runtimeMode", "approval-required"},
                                 {"workspaceStrategy", {{"type", "worktree"}, {"baseRef", pin}, {"startFromOrigin", false}}}});
        check(provisioned.at("threadId") == command.actionId && provisioned.at("runId").is_null(),
              "fixture did not leave a provision-only launch for same-key recovery");
        const auto started = adapter->send(command);
        check(started.kind == Delivery::Kind::Accepted && started.run.threadId == command.actionId && started.run.runId == "next-run",
              "idle stock workspace starts only after its input pin matches");

        fixture({{"attachmentTools", true}});
        auto refused = command;
        refused.actionId = "pinned-refused-after-provision";
        refused.attachments = {{"report.txt", "text/plain", "Pinned report"}};
        refused.attachmentCount = refused.attachments.size();
        auto pendingAttachments = [&] {
            const auto response = http(origin + "/fixture/attachments", "GET");
            check(response.status == 200, "Could not inspect staged files on the T3 fixture");
            return Json::parse(response.body).at("pending").get<std::size_t>();
        };
        auto effectCount = [&](std::string_view name) {
            const auto current = Json::parse(http(origin + "/fixture/effects", "GET").body);
            const auto found = current.find(name);
            return found == current.end() ? std::size_t{} : found->size();
        };
        const auto pendingBeforeRefusal = pendingAttachments();
        const auto sendsBeforeRefusal = effectCount("t3_thread_send_attachments");
        const auto discardsBeforeRefusal = effectCount("t3_attachment_discard");
        int authorizationChecks = 0;
        const auto deniedAfterProvision = adapter->send(refused, [&] { return ++authorizationChecks == 1; });
        const auto stockAfterRefusal = Json::parse(http(origin + "/fixture/stock", "GET").body);
        check(deniedAfterProvision.kind == Delivery::Kind::Settings && deniedAfterProvision.definitelyUnsent &&
                  deniedAfterProvision.noAutomaticRetry,
              "a pinned pre-send authorization refusal was not terminal: kind=" +
                  std::to_string(static_cast<int>(deniedAfterProvision.kind)) + ", definitelyUnsent=" +
                  std::to_string(deniedAfterProvision.definitelyUnsent) + ", noAutomaticRetry=" +
                  std::to_string(deniedAfterProvision.noAutomaticRetry) + ", checks=" +
                  std::to_string(authorizationChecks) + ", detail=" + deniedAfterProvision.detail);
        check(authorizationChecks == 2 && stockAfterRefusal.at("stockLaunches").contains("project:pinned-refused-after-provision"),
              "the authorization refusal did not occur after pinned Session creation");
        check(effectCount("t3_thread_send_attachments") == sendsBeforeRefusal,
              "the pinned pre-send refusal dispatched its staged attachments");
        check(effectCount("t3_attachment_discard") == discardsBeforeRefusal + 1 &&
                  pendingAttachments() == pendingBeforeRefusal,
              "the pinned pre-send refusal retained staged attachments");

        auto ambiguous = refused;
        ambiguous.actionId = "pinned-ambiguous-attachment-send";
        const auto pendingBeforeAmbiguous = pendingAttachments();
        const auto sendsBeforeAmbiguous = effectCount("t3_thread_send_attachments");
        const auto discardsBeforeAmbiguous = effectCount("t3_attachment_discard");
        fixture({{"dropAttachmentReply", true}});
        authorizationChecks = 0;
        const auto lostReply = adapter->send(ambiguous, [&] { ++authorizationChecks; return true; });
        check(lostReply.kind == Delivery::Kind::Unknown && authorizationChecks == 2 &&
                  effectCount("t3_thread_send_attachments") == sendsBeforeAmbiguous + 1 &&
                  effectCount("t3_attachment_discard") == discardsBeforeAmbiguous &&
                  pendingAttachments() == pendingBeforeAmbiguous + 1,
              "an ambiguous pinned attachment send was discarded or replayed");

        for (const auto mode : {"unstaged", "staged"}) {
            fixture({{"stockTrackedEdit", mode}});
            const auto dirty = adapter->observe(environment, command.config, started.run);
            check(dirty.kind == Observation::Kind::Finished && dirty.status == "failed" && !dirty.report.empty(),
                  "tracked " + std::string(mode) + " edit did not produce terminal failed evidence");
            fixture({{"stockTrackedEdit", "clean"}});
        }
        const auto report = adapter->observe(environment, command.config, started.run);
        check(report.kind == Observation::Kind::Finished && report.status == "completed" && report.report == "Pinned report",
              "provider report from the pinned workspace is accepted");
        fixture({{"advanceStockWorker", true}});
        const auto changed = adapter->observe(environment, command.config, started.run);
        check(changed.kind == Observation::Kind::Finished && changed.status == "failed" && changed.report != "Pinned report",
              "settled specialist result is invalidated when the actual workspace drifted");
        command.actionId = "wrong-worker";
        const auto wrong = adapter->send(command);
        check(wrong.kind == Delivery::Kind::NotAccepted && wrong.definitelyUnsent,
              "different actual worktree input blocks the first prompt");
        const auto effects = Json::parse(http(origin + "/fixture/effects", "GET").body);
        const auto stock = Json::parse(http(origin + "/fixture/stock", "GET").body);
        check(effects.at("t3_thread_send").size() == 1 && effects.at("t3_thread_launch").size() == 7 &&
                  !effects.contains("t3_thread_interrupt") && stock.at("stockLaunches").size() == 6 &&
                  stock.at("stockLaunches").at("project:pinned-worker").at("replays") == 1 &&
                  stock.at("stockPromptSends").at("pinned-worker:pinned-worker:start").at("replays") == 0,
              "same-key provision recovery created another thread, resent the first prompt, or sent to invalid work");

        std::cout << "Pinned T3 workspace verification passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
