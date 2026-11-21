#include "internal.hpp"

#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace flockv0;
using flockv0::t3detail::http;

namespace {
void check(bool pass, std::string_view reason) {
    if (!pass) throw std::logic_error(std::string(reason));
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
        auto effects = [&](std::string_view tool) {
            const auto response = http(origin + "/fixture/effects", "GET");
            check(response.status == 200, "could not read fixture effects");
            return Json::parse(response.body).value(std::string(tool), Json::array());
        };
        auto signIn = [&](const std::string& environment) {
            loginT3(home, environment, endpoint, [&](const std::string& authorization) {
                fixture({{"authorization", authorization}});
            });
        };
        signIn("env-1");
        auto ownerOnly = [](const std::filesystem::path& path) {
            struct stat st{};
            return stat(path.c_str(), &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 0077) == 0;
        };
        check(ownerOnly(home), "credential directory is owner-only");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(home))
            check(ownerOnly(entry.path()), "stored credentials are owner-only");
        auto adapter = makeT3Mcp(home);
        Command launch;
        launch.actionId = "flock-action-0123";
        launch.tool = "delegate";
        launch.environmentId = "env-1";
        launch.projectId = "project";
        launch.instruction = "  preserve instruction whitespace and content  ";
        launch.authGeneration = adapter->authGeneration(launch.environmentId, launch.config);
        check(!launch.authGeneration.empty() && !adapter->preflight(launch), "valid credential preflight");
        auto checkPreflight = [&](const Command& command, Delivery::Kind kind, std::string_view reason) {
            const auto failure = adapter->preflight(command);
            check(failure && failure->kind == kind, reason);
        };
        auto restarted = makeT3Mcp(home);
        check(restarted->authGeneration("env-1", launch.config) == launch.authGeneration, "persistent OAuth generation");
        auto other = launch;
        other.environmentId = "env-2";
        checkPreflight(other, Delivery::Kind::Auth, "another environment cannot use credential");
        other = launch;
        other.config["t3"]["mcpUrl"] = "https://example.test/mcp";
        checkPreflight(other, Delivery::Kind::Settings, "URL hint cannot redirect credential");
        other = launch;
        other.config["session"]["model"] = "missing provider";
        checkPreflight(other, Delivery::Kind::Settings, "partial model selection rejected before send");
        fixture({{"delayRunId", true}});
        auto delivery = adapter->send(launch);
        check(delivery.kind == Delivery::Kind::Unknown && delivery.run.threadId == "thread", "delayed Run ID remains unconfirmed");
        delivery = restarted->reconcile(launch);
        check(delivery.kind == Delivery::Kind::Accepted && delivery.run.threadId == "thread" &&
              delivery.run.runId == "root", "recovered exact launch after restart");
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
        const auto launches = effects("t3_thread_launch");
        check(launches.size() == 1, "launch reconciliation creates no additional T3 threads");
        check(launches.at(0).at("message").get<std::string>().find(launch.instruction) != std::string::npos,
              "T3 receives the complete original instruction with its whitespace");

        fixture({{"scenario", "question"}});
        auto observation = adapter->observe("env-1", launch.config, run);
        check(observation.kind == Observation::Kind::Question && observation.question.requestId == "request" &&
              observation.question.fields.size() == 1, "owned question is exposed with its field");
        const auto& field = observation.question.fields.at(0);
        check(field.at("id") == "color" && field.at("question") == "Which colors?" && field.at("multiple") == true &&
              field.at("freeform") == false, "question retains its prompt and input constraints");
        const auto& options = field.at("options");
        check(options.size() == 2 && options.at(0).at("label") == "Green" && options.at(0).at("value") == "green" &&
              options.at(1).at("label") == "blue" && options.at(1).at("value") == "blue",
              "question retains the display labels and answer values for both options");
        Command answer = launch;
        answer.tool = "answer_session_question";
        answer.target = run;
        answer.questionId = "request";
        answer.answers = {{"color", "missing option"}};
        check(adapter->send(answer).kind == Delivery::Kind::NotAccepted, "invalid option is rejected");
        answer.answers = {{"color", Json::array({"green", "blue"})}};
        fixture({{"scenario", "ambiguous"}});
        check(adapter->observe("env-1", launch.config, run).kind == Observation::Kind::ExternalInput &&
              adapter->send(answer).kind == Delivery::Kind::NotAccepted, "identical content in another Run blocks auto-answer");
        fixture({{"scenario", "approval"}});
        check(adapter->observe("env-1", launch.config, run).kind == Observation::Kind::ExternalInput, "approvals stay in T3");
        fixture({{"scenario", "question"}});
        check(adapter->reconcile(answer).kind == Delivery::Kind::Unknown, "reconciliation reports the pending question");
        check(effects("t3_pending_request").empty(),
              "T3 receives no reply for an invalid option, ambiguous ownership or unknown delivery");
        check(adapter->send(answer).kind == Delivery::Kind::Accepted &&
              restarted->reconcile(answer).kind == Delivery::Kind::QuestionGone &&
              restarted->send(answer).kind == Delivery::Kind::QuestionGone, "answer uses original request once");
        const auto replies = effects("t3_pending_request");
        check(replies.size() == 1, "T3 receives exactly one reply across restart and retry");
        check(replies.at(0).at("threadId") == "thread" && replies.at(0).at("requestId") == "request" &&
              replies.at(0).at("answers") == Json{{"color", Json::array({"green", "blue"})}},
              "selected values are sent to the original T3 question");
        fixture({{"scenario", "race"}});
        check(adapter->send(answer).kind == Delivery::Kind::QuestionGone, "user answer during dispatch restores monitoring");

        fixture({{"scenario", "settling"}});
        observation = adapter->observe("env-1", launch.config, run);
        check(observation.kind == Observation::Kind::Finished && !observation.settled, "wait timeout preserves outstanding work");
        const auto checkResult = [&](const char* scenario, bool ownsLater) {
            fixture({{"scenario", scenario}});
            const auto observed = adapter->observe("env-1", launch.config, run);
            check(observed.settled && observed.resultRunId == (ownsLater ? "later" : "root") &&
                  observed.report == (ownsLater ? "later report" : "root report"),
                  std::string("Incorrect result attribution in ") + scenario);
        };
        checkResult("notification-result", true);
        checkResult("shortened-result", true);
        checkResult("nested-result", true);
        checkResult("mixed-result", false);
        checkResult("server-input-result", false);
        checkResult("ambiguous-title-result", false);
        checkResult("reused-result", false);

        Command follow = launch;
        follow.actionId = "flock-action-01234";
        follow.tool = "follow_up";
        follow.target = run;
        follow.instruction = std::string(700, 'x');
        for (const auto* scenario : {"manual-active", "server-input-result"}) {
            fixture({{"scenario", scenario}});
            check(adapter->send(follow).kind == Delivery::Kind::NotAccepted,
                  std::string("Follow-up accepted in ") + scenario);
        }
        check(effects("t3_thread_send").empty(), "T3 received a follow-up for a busy or unrelated Run");
        fixture({{"scenario", "settled"}});
        delivery = adapter->send(follow);
        check(delivery.kind == Delivery::Kind::Accepted && delivery.run.threadId == "thread" &&
              delivery.run.runId == "follow", "follow-up starts a new Run in the original thread");
        delivery = restarted->reconcile(follow);
        check(delivery.kind == Delivery::Kind::Accepted && delivery.run.threadId == "thread" &&
              delivery.run.runId == "follow", "reconciliation recovers the follow-up from paginated history");
        const auto followUps = effects("t3_thread_send");
        check(followUps.size() == 1, "follow-up reconciliation does not send another queued message");
        check(followUps.at(0).at("message").get<std::string>().find(follow.instruction) != std::string::npos,
              "T3 receives all of the follow-up instruction");

        ReadRequest read;
        read.environmentId = "env-1";
        read.threadId = "thread";
        read.runId = "root";
        read.itemId = "unicode";
        std::string expectedText = "界";
        for (int i = 0; i < 3000; ++i) expectedText += "🙂";
        expectedText += "犬";
        for (int i = 0; i < 3000; ++i) expectedText += "🐦";
        expectedText += "\n夕焼け";
        std::string receivedText;
        Excerpt chunk;
        do {
            chunk = adapter->read(read);
            check(!chunk.text.empty() && chunk.text.size() <= 8192, "each Unicode excerpt fits the byte limit");
            check(chunk.threadId == "thread" && chunk.runId == "root" && chunk.itemId == "unicode",
                  "continuation retains the excerpt's thread, Run and item");
            receivedText += chunk.text;
            check(receivedText.size() <= expectedText.size(), "continuation does not repeat text");
            check(!chunk.more || chunk.nextOffset > read.offset, "a continued excerpt advances its cursor");
            read.offset = chunk.nextOffset;
        } while (chunk.more);
        check(receivedText == expectedText, "continued excerpts preserve the complete Unicode text");
        const auto boundaries = effects("unicode_split_avoided");
        check(!boundaries.empty(), "the fixture did not encounter a split surrogate pair");
        for (const auto& boundary : boundaries) {
            check(boundary.at("nextOffset").get<std::size_t>() + 1 ==
                  boundary.at("offset").get<std::size_t>() + boundary.at("limit").get<std::size_t>(),
                  "the fixture did not back up from the requested limit by one UTF-16 code unit");
        }
        ReadRequest translation = read;
        translation.itemId = "translation";
        translation.offset = 0;
        std::string expectedTranslation;
        for (int i = 0; i < 2800; ++i) expectedTranslation += "—";
        std::string receivedTranslation;
        do {
            chunk = adapter->read(translation);
            check(chunk.itemId == "translation" && !chunk.text.empty() && chunk.text.size() <= 8192,
                  "translated fixture excerpts retain their item and byte boundary");
            receivedTranslation += chunk.text;
            translation.offset = chunk.nextOffset;
        } while (chunk.more);
        check(receivedTranslation == expectedTranslation,"English fixture translation was not preserved");
        ReadRequest denseBmp = read;
        denseBmp.runId = "root";
        denseBmp.itemId = "dense-bmp";
        denseBmp.offset = 0;
        std::string expectedBmp;
        for (int i = 0; i < 7000; ++i) expectedBmp += "界";
        std::string receivedBmp;
        do {
            chunk = adapter->read(denseBmp);
            check(chunk.itemId == "dense-bmp" && !chunk.text.empty() && chunk.text.size() <= 8192,
                  "dense BMP excerpts stay within the byte limit");
            receivedBmp += chunk.text;
            check(receivedBmp.size() <= expectedBmp.size(), "dense BMP continuation does not repeat text");
            check(!chunk.more || chunk.nextOffset > denseBmp.offset, "dense BMP continuation advances its cursor");
            denseBmp.offset = chunk.nextOffset;
        } while (chunk.more);
        check(receivedBmp == expectedBmp, "dense BMP excerpts preserve the complete text");
        read.offset = 0;
        read.runId = "wrong";
        bool rejected = false;
        try { adapter->read(read); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "foreign item cannot be read");
        adapter->interrupt("env-1", launch.config, run);
        check(effects("t3_thread_interrupt").empty(), "T3 receives no interrupt for an already settled Run");
        fixture({{"scenario", "running"}});
        adapter->interrupt("env-1", launch.config, run);
        check(effects("t3_thread_interrupt") == Json::array({{{"threadId", "thread"}, {"runId", "root"}}}),
              "T3 receives exactly one interrupt for the assigned Run");

        signIn("env-1");
        const auto reauthorized = adapter->authGeneration("env-1", launch.config);
        check(reauthorized != launch.authGeneration, "reauthorization changes generation");
        other = launch;
        other.authGeneration = reauthorized;
        check(!adapter->preflight(other), "reauthorization retains permission to operate");
        const auto staleAuth = adapter->send(launch);
        check(staleAuth.kind == Delivery::Kind::Auth && staleAuth.definitelyUnsent,
              "stale generation blocks commands before send");
        check(effects("t3_thread_launch").size() == 1, "stale authorization never launches another T3 thread");

        fixture({{"scope", "orchestration:read"}});
        signIn("env-1");
        other.authGeneration = adapter->authGeneration("env-1", launch.config);
        checkPreflight(other, Delivery::Kind::Auth, "read-only OAuth cannot operate");
        check(adapter->send(other).kind == Delivery::Kind::Auth &&
              effects("t3_thread_launch").size() == 1, "read-only authorization never launches a T3 thread");
        delivery = adapter->reconcile(launch);
        check(delivery.kind == Delivery::Kind::Accepted && delivery.run.threadId == "thread" &&
              delivery.run.runId == "root", "reauthorization with read scope recovers the original Run");
        const auto readOnly = other.authGeneration;
        fixture({{"revoked", true}});
        observation = adapter->observe("env-1", launch.config, run);
        check(observation.kind == Observation::Kind::Unavailable && observation.status == "auth",
              "server-side OAuth revocation is surfaced");
        rejected = false;
        try { signIn("env-2"); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "OAuth login rejects a different canonical environment ID");
        other = launch;
        other.environmentId = "env-2";
        checkPreflight(other, Delivery::Kind::Auth, "mismatched login leaves no usable credential");
        check(adapter->authGeneration("env-1", launch.config) == readOnly,
              "mismatched login preserves the credential for the canonical environment");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
