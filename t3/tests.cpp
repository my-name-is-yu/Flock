#include "internal.hpp"

#include <iostream>
#include <sys/stat.h>

using namespace flockv0;
using namespace flockv0::t3detail;

namespace {
void check(bool pass, std::string_view reason) {
    if (!pass) throw std::runtime_error(std::string(reason));
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3, "expected fixture URL and credential directory");
        const std::string origin = argv[1];
        const auto endpoint = origin + "/mcp";
        const std::filesystem::path home = argv[2];
        auto fixture = [&](Json configuration) {
            check(http(origin + "/fixture", "POST", {{"Content-Type", "application/json"}},
                       configuration.dump()).status == 200, "fixture rejected configuration");
        };
        auto signIn = [&](const std::string& environment) {
            loginT3(home, environment, endpoint, [&](const std::string& authorization) {
                fixture({{"authorization", authorization}});
            });
        };
        signIn("env-1");
        struct stat st{};
        check(stat(home.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700, "credential directory permissions");
        int files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(home)) {
            check(stat(entry.path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "credential file permissions");
            ++files;
        }
        check(files == 1, "one credential file per environment");
        auto adapter = makeT3Mcp(home);
        Command launch;
        launch.actionId = "flock-action-0123";
        launch.tool = "delegate";
        launch.environmentId = "env-1";
        launch.projectId = "project";
        launch.instruction = "  preserve instruction whitespace and content  ";
        launch.authGeneration = adapter->authGeneration(launch.environmentId, launch.config);
        check(!launch.authGeneration.empty() && !adapter->preflight(launch), "valid credential preflight");
        auto restarted = makeT3Mcp(home);
        check(restarted->authGeneration("env-1", launch.config) == launch.authGeneration, "persistent OAuth generation");
        auto other = launch;
        other.environmentId = "env-2";
        check(adapter->preflight(other)->kind == Delivery::Kind::Auth, "another environment cannot use credential");
        other = launch;
        other.config["t3"]["mcpUrl"] = "https://example.test/mcp";
        check(adapter->preflight(other)->kind == Delivery::Kind::Settings, "URL hint cannot redirect credential");
        other = launch;
        other.config["session"]["model"] = "missing provider";
        check(adapter->preflight(other)->kind == Delivery::Kind::Settings, "partial model selection rejected before send");
        fixture({{"delayRunId", true}});
        auto delivery = adapter->send(launch);
        check(delivery.kind == Delivery::Kind::Unknown && delivery.run.threadId == "thread", "delayed Run ID remains unconfirmed");
        delivery = restarted->reconcile(launch);
        check(delivery.kind == Delivery::Kind::Accepted && delivery.run.runId == "root", "recovered exact launch after restart");
        const auto run = delivery.run;
        other = launch;
        other.instruction += "x";
        check(adapter->reconcile(other).kind == Delivery::Kind::Unknown, "marker requires identical instruction");
        other = launch;
        other.candidate = {"thread", "wrong", {}};
        check(adapter->reconcile(other).kind == Delivery::Kind::Unknown, "manual candidate does not override identity");
        fixture({{"duplicate", true}});
        check(adapter->reconcile(launch).kind == Delivery::Kind::Unknown, "multiple matching threads are ambiguous");
        fixture({{"duplicate", false}});

        fixture({{"scenario", "question"}});
        auto observation = adapter->observe("env-1", launch.config, run);
        check(observation.kind == Observation::Kind::Question && observation.question.requestId == "request" &&
              observation.question.fields[0].at("multiple") == true &&
              observation.question.fields[0].at("freeform") == false, "question is owned and preserves fields");
        Command answer = launch;
        answer.tool = "answer_session_question";
        answer.target = run;
        answer.questionId = "request";
        answer.answers = {{"color", "missing option"}};
        check(adapter->send(answer).kind == Delivery::Kind::NotAccepted, "invalid option is never sent");
        answer.answers = {{"color", Json::array({"green", "blue"})}};
        fixture({{"scenario", "ambiguous"}});
        check(adapter->observe("env-1", launch.config, run).kind == Observation::Kind::ExternalInput &&
              adapter->send(answer).kind == Delivery::Kind::NotAccepted, "identical content in another Run blocks auto-answer");
        fixture({{"scenario", "approval"}});
        check(adapter->observe("env-1", launch.config, run).kind == Observation::Kind::ExternalInput, "approvals stay in T3");
        fixture({{"scenario", "question"}});
        check(adapter->reconcile(answer).kind == Delivery::Kind::Unknown, "unknown reply is never replayed");
        check(adapter->send(answer).kind == Delivery::Kind::Accepted &&
              restarted->reconcile(answer).kind == Delivery::Kind::QuestionGone &&
              restarted->send(answer).kind == Delivery::Kind::QuestionGone, "answer uses original request once");
        fixture({{"scenario", "race"}});
        check(adapter->send(answer).kind == Delivery::Kind::QuestionGone, "user answer during dispatch restores monitoring");

        fixture({{"scenario", "settling"}});
        observation = adapter->observe("env-1", launch.config, run);
        check(observation.kind == Observation::Kind::Finished && !observation.settled, "wait timeout preserves outstanding work");
        fixture({{"scenario", "manual-result"}});
        observation = adapter->observe("env-1", launch.config, run);
        check(observation.settled && observation.resultRunId == "root" && observation.report == "root report",
              "unrelated manual result is not attributed to Flock");
        fixture({{"scenario", "delegated-result"}});
        observation = adapter->observe("env-1", launch.config, run);
        check(observation.settled && observation.resultRunId == "later" && observation.report == "delegated report",
              "delegate result is tied to owned subagent activity");

        Command follow = launch;
        follow.actionId = "flock-action-01234";
        follow.tool = "follow_up";
        follow.target = run;
        follow.instruction = std::string(700, 'x');
        fixture({{"scenario", "manual-active"}});
        check(adapter->send(follow).kind == Delivery::Kind::NotAccepted, "busy thread blocks queue send");
        fixture({{"scenario", "settled"}});
        check(adapter->send(follow).kind == Delivery::Kind::Accepted, "follow-up uses normal queue send");
        check(restarted->reconcile(follow).kind == Delivery::Kind::Accepted, "follow-up reconciliation follows a truncated action item");

        ReadRequest read;
        read.environmentId = "env-1";
        read.threadId = "thread";
        read.runId = "root";
        read.itemId = "unicode";
        auto chunk = adapter->read(read);
        check(chunk.more && chunk.text.size() == 8190 && chunk.nextOffset == 2730, "excerpt caps bytes and preserves UTF-16 cursor");
        read.offset = chunk.nextOffset;
        chunk = adapter->read(read);
        check(!chunk.more && chunk.text.size() == 210, "excerpt continuation remains in the same item");
        read.runId = "wrong";
        bool rejected = false;
        try { adapter->read(read); } catch (const Failure&) { rejected = true; }
        check(rejected, "foreign item cannot be read");
        fixture({{"scenario", "running"}});
        adapter->interrupt("env-1", launch.config, run);

        fixture({{"scope", "orchestration:read"}});
        signIn("env-1");
        check(adapter->authGeneration("env-1", launch.config) != launch.authGeneration, "reauthorization changes generation");
        check(adapter->preflight(launch)->kind == Delivery::Kind::Auth, "read-only OAuth cannot operate");
        check(adapter->send(launch).kind == Delivery::Kind::NotAccepted, "stale generation blocks commands before send");
        check(adapter->reconcile(launch).kind == Delivery::Kind::Accepted, "reauthorization can recover original Run");
        fixture({{"revoked", true}});
        check(adapter->observe("env-1", launch.config, run).status == "auth", "server-side OAuth revocation is surfaced");
        rejected = false;
        try { signIn("env-2"); } catch (const Failure&) { rejected = true; }
        check(rejected, "OAuth cannot be saved under a different canonical environment ID");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
