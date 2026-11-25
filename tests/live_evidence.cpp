#include "flock/t3_mcp.hpp"
#include "live_setup.hpp"
#include "live_t3_fixture.hpp"
#include "model.hpp"
#include "flock/decision.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_session.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace flockv0;

namespace {
const std::string setup =
    std::string(live_test::chatGPTSetup) + live_test::t3Setup +
    "Set FLOCK_LIVE_COMMIT to the full SHA checked out in the T3 fixture project.\n"
    "Provide executable measure_on.sh and measure_off.sh in that project's root.\n"
    "Each script must measure peak RSS and print FLOCK_MEASUREMENT commit=<commit> updates=<on|off> peak_rss=<integer>.\n";

void usage(std::ostream& out) {
    out << "Usage:\n"
        << "  flock_live_evidence --live\n"
        << "  flock_live_evidence <FLOCK_HOME> <db> <matchingTaskId> <mismatchedTaskId> <model> <commit>\n"
        << "  flock_live_evidence --self-test\n\n"
        << "--live creates and runs both measurement tasks in a temporary database.\n"
        << "For an existing database, pass completed measurement-match and measurement-mismatch tasks from flock_live_t3.\n"
        << "--self-test checks the six verdict expectations and on-demand tool schema offline.\n\n"
        << setup;
}

struct Recording {
    Json event;
    ReadRequest location;
    Excerpt evidence;
};

bool containsMeasurement(std::string_view text, std::string_view marker) {
    for (auto match = text.find(marker); match != std::string_view::npos; match = text.find(marker, match + marker.size())) {
        const auto value = text.substr(match + marker.size());
        std::uint64_t rss;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), rss);
        if (error == std::errc{} &&
            (end == value.data() + value.size() || std::string_view(" \t\r\n").find(*end) != std::string_view::npos))
            return true;
    }
    return false;
}

Recording load(Store& store, T3& t3, Id id, std::string_view commit, bool matches) {
    const auto task = taskJson(store.task(id));
    Json report;
    for (const auto& item : store.history(id)) {
        if (item["kind"] == "report") report = item["data"];
    }
    if (task.at("state") != "completed" || report.is_null() || report.at("status") != "completed" ||
        report.at("report") != "Measurement complete")
        throw Conflict("Expected a completed fixture with the final report 'Measurement complete'");

    const auto& run = report.at("run");
    ReadRequest request;
    request.environmentId = report.at("environmentId");
    request.gatewayEnvironmentId = run.value("gatewayEnvironmentId", "");
    request.threadId = run.at("threadId");
    request.runId = report.at("resultRunId");
    request.view = "activity";
    request.config = task.at("config");
    const auto marker = "FLOCK_MEASUREMENT commit=" + std::string(commit) + " updates=" + (matches ? "on" : "off") + " peak_rss=";
    for (int page = 0; page < 30; ++page) {
        const auto excerpt = t3.read(request);
        if (!excerpt.itemId.empty() && containsMeasurement(excerpt.text, marker)) {
            request.cursor.clear();
            request.itemId = excerpt.itemId;
            const auto exact = t3.read(request);
            if (exact.text != excerpt.text) throw Conflict("The chosen T3 item changed");
            return {std::move(report), std::move(request), exact};
        }
        if (!excerpt.more || excerpt.nextCursor.empty() || request.cursor == excerpt.nextCursor) break;
        request.cursor = excerpt.nextCursor;
    }
    throw Conflict("T3 activity did not contain a measurement starting with " + marker + " followed by a numeric peak RSS");
}

Json locator(const ReadRequest& location) {
    return {{"environmentId", location.environmentId},
            {"threadId", location.threadId},
            {"runId", location.runId},
            {"itemId", location.itemId},
            {"view", location.view}};
}

Json evidenceOf(const Recording& recording) {
    auto result = locator(recording.location);
    result["text"] = recording.evidence.text;
    result["more"] = recording.evidence.more;
    return result;
}

struct Condition {
    std::string_view mode;
    bool measurementMatches;

    std::vector<std::string_view> expected() const {
        if (measurementMatches && mode != "off") return {"complete"};
        return {"follow_up", "ask_user"};
    }
};

std::string_view verdictError(const Decision& decision, const std::vector<std::string_view>& expected, bool hasEvidence) {
    if (std::ranges::find(expected, decision.tool) == expected.end()) return "Unexpected verdict";
    const char* textField = "question";
    if (decision.tool == "complete") {
        if (!hasEvidence) return "Completion requires measurement evidence";
        textField = "report";
    } else if (decision.tool == "follow_up") {
        textField = "instruction";
    }
    const auto text = decision.args.find(textField);
    if (text == decision.args.end() || !text->is_string() ||
        text->get_ref<const std::string&>().find_first_not_of(" \t\r\n") == std::string::npos)
        return "Verdict is missing its required nonblank text";
    return {};
}

ModelInput modelInput(const Recording& recording, std::string_view commit, std::string_view slug, std::string_view mode) {
    const auto criterion =
        "At commit " + std::string(commit) + ", measure peak RSS for the build with updates enabled and report the numeric result.";
    ModelInput input{
        {{"id", 1}, {"botId", "memory"}, {"request", criterion}, {"doneWhen", criterion}, {"remaining", 3}, {"additionalUsed", 0}},
        recording.event,
        Json::array(),
        {"Validate the measured commit, setting and numeric RSS with evidence. "
         "The generic final report alone cannot establish the criteria. "
         "Use a supplied build-output item as evidence. If it has not been supplied and session_read is available, "
         "read that item using the threadId, runId, itemId and view in buildOutput. "
         "If evidence remains insufficient, follow up for the missing evidence.",
         "buildOutput identifies the terminal activity for the measurement."},
        {"follow_up", "complete", "ask_user"},
        "",
        Json::array(),
        {{"modelProvider", "chatgpt"}, {"model", slug}}};
    input.event["buildOutput"] = locator(recording.location);
    if (mode == "on_demand") {
        input.event["sessionReadViews"] = Json::array({"messages", "activity"});
        input.tools.push_back("session_read");
    } else if (mode == "prefill") {
        input.evidence.push_back(evidenceOf(recording));
    }
    return input;
}

bool evaluate(T3& t3, Model& model, const Recording& recording, std::string_view commit, std::string_view slug, Condition condition) {
    const auto expected = condition.expected();
    Json result = {{"mode", condition.mode},
                   {"matchingMeasurement", condition.measurementMatches},
                   {"expected", expected},
                   {"turns", Json::array()},
                   {"pass", false}};
    try {
        auto input = modelInput(recording, commit, slug, condition.mode);
        constexpr std::size_t maxReads = 3;
        while (true) {
            const auto turn = model.decide(input);
            if (turn.calls.size() != 1) throw Conflict("Expected one task action");
            const auto& chosen = turn.calls.front();
            result["turns"].push_back({{"tool", chosen.tool},
                                       {"reason", chosen.reason},
                                       {"usage", turn.usage},
                                       {"inputBytes", (input.task.dump() + input.event.dump() + input.evidence.dump()).size()}});
            if (chosen.tool != "session_read") {
                result["action"] = chosen.tool;
                result["args"] = chosen.args;
                const auto error = verdictError(chosen, expected, !input.evidence.empty());
                result["pass"] = error.empty();
                if (!error.empty()) result["error"] = error;
                break;
            }
            const auto& location = recording.location;
            if (chosen.args.at("threadId") != location.threadId || chosen.args.at("runId") != location.runId ||
                chosen.args.at("itemId") != location.itemId || chosen.args.at("view") != location.view ||
                chosen.args.value("cursor", "") != "" || chosen.args.value("offset", Json(0)) != 0)
                throw Conflict("Expected session_read to select the recorded activity item from its start");
            const auto fetched = t3.read(location);
            if (fetched.text != recording.evidence.text) throw Conflict("T3 returned different evidence on demand");
            input.evidence.push_back(evidenceOf(recording));
            if (input.evidence.size() == maxReads) {
                std::erase(input.tools, "session_read");
                input.event.erase("sessionReadViews");
            }
        }
    } catch (const std::exception& error) {
        result["error"] = error.what();
    }
    std::cout << result.dump() << std::endl;
    return result.at("pass").get<bool>();
}

int runTrials(Store& store, T3& t3, Model& model, Id matching, Id mismatched, std::string_view slug, std::string_view commit) {
    bool passed = true;
    for (const bool matches : {true, false}) {
        try {
            const auto recording = load(store, t3, matches ? matching : mismatched, commit, matches);
            for (const std::string_view mode : {"off", "on_demand", "prefill"}) {
                if (!evaluate(t3, model, recording, commit, slug, {mode, matches})) passed = false;
            }
        } catch (const std::exception& error) {
            std::cerr << (matches ? "Matching" : "Mismatched") << " measurement fixture failed: " << error.what() << '\n';
            passed = false;
        }
    }
    return passed ? 0 : 1;
}

void selfTest() {
    const std::vector<std::string_view> complete{"complete"};
    const std::vector<std::string_view> moreEvidence{"follow_up", "ask_user"};
    if (Condition{"off", true}.expected() != moreEvidence || Condition{"off", false}.expected() != moreEvidence ||
        Condition{"on_demand", true}.expected() != complete || Condition{"on_demand", false}.expected() != moreEvidence ||
        Condition{"prefill", true}.expected() != complete || Condition{"prefill", false}.expected() != moreEvidence)
        throw Conflict("The live evidence verdict expectations changed");
    const Decision completed{"complete", {{"report", "Peak RSS was 42"}}, "The measurement matches"};
    if (!verdictError(completed, complete, true).empty() || verdictError(completed, complete, false).empty() ||
        verdictError(completed, moreEvidence, true).empty() || verdictError(Decision{"complete"}, complete, true).empty() ||
        verdictError(Decision{"follow_up"}, moreEvidence, false).empty() ||
        verdictError(Decision{"ask_user", {{"question", " "}}, "Missing evidence"}, moreEvidence, false).empty())
        throw Conflict("The live evidence verdict must be usable and support completion with evidence");

    const Recording recording{{{"kind", "result"}, {"status", "completed"}, {"report", "Measurement complete"}},
                              {.environmentId = "fixture-env",
                               .threadId = "fixture-thread",
                               .runId = "fixture-run",
                               .itemId = "fixture-item",
                               .view = "activity"},
                              {}};
    const auto input = modelInput(recording, "fixture-commit", "fixture-model", "on_demand");
    const auto request = app::responseRequest(input);
    const auto& tools = request.at("tools").at(0).at("tools");
    const auto read = std::ranges::find_if(tools, [](const Json& tool) { return tool.at("name") == "session_read"; });
    if (read == tools.end() || read->at("parameters").at("properties").at("view").at("enum") != Json::array({"messages", "activity"}))
        throw Conflict("On-demand input must expose the production session_read views");
    auto args = locator(recording.location);
    args.erase("environmentId");
    const auto output = Json::array({{{"type", "function_call"},
                                      {"namespace", "flock"},
                                      {"status", "completed"},
                                      {"name", "session_read"},
                                      {"arguments", args.dump()}}});
    const auto decision = app::responseDecision(output, request);
    if (decision.calls.size() != 1 || decision.calls.front().tool != "session_read" || decision.calls.front().args != args)
        throw Conflict("On-demand session_read arguments did not pass production tool validation");
}

bool validCommit(std::string_view commit) {
    return (commit.size() == 40 || commit.size() == 64) && commit.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
}

Id taskId(std::string_view text) {
    Id id;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (error != std::errc{} || end != text.data() + text.size() || id < 1) throw Conflict("Task IDs must be positive integers");
    return id;
}

int runLive() {
    const auto home = live_test::environment("FLOCK_HOME");
    const auto slug = live_test::environment("FLOCK_LIVE_MODEL");
    const auto environment = live_test::environment("FLOCK_LIVE_T3_ENVIRONMENT");
    const auto project = live_test::environment("FLOCK_LIVE_T3_PROJECT");
    const auto commit = live_test::environment("FLOCK_LIVE_COMMIT");
    if (home.empty()) return live_test::skip("FLOCK_HOME is unset", setup);
    if (slug.empty()) return live_test::skip("FLOCK_LIVE_MODEL is unset", setup);
    if (environment.empty()) return live_test::skip("FLOCK_LIVE_T3_ENVIRONMENT is unset", setup);
    if (project.empty()) return live_test::skip("FLOCK_LIVE_T3_PROJECT is unset", setup);
    if (commit.empty()) return live_test::skip("FLOCK_LIVE_COMMIT is unset", setup);
    if (!validCommit(commit)) throw Conflict("FLOCK_LIVE_COMMIT must be a full Git commit SHA");

    const auto credentials = std::filesystem::path(home) / "credentials";
    if (!live_test::hasT3Credentials(credentials / "t3")) return live_test::skip("T3 credentials are missing.", setup);
    if (!live_test::hasChatGPTProfile(credentials / "chatgpt")) return live_test::skip("ChatGPT credentials are missing.", setup);
    app::ChatGPTModel model(credentials / "chatgpt");
    const auto status = model.status();
    if (status.at("connected") == false) return live_test::skip("ChatGPT credentials are missing.", setup);
    if (status.at("planUsage") == false) throw Conflict("Run flock model login and grant ChatGPT plan usage");

    auto t3 = makeT3Mcp(credentials / "t3");
    t3->authGeneration(environment, Json::object());
    live_test::ScratchDirectory scratch;
    Store store(scratch.path / "state.db");
    const auto run = [&](std::string_view scenario) {
        const auto id = live_test::submit(store, environment, project, scenario);
        if (live_test::pump(store, *t3, id, "completed") != 0) throw Conflict(std::string(scenario) + " did not complete");
        return id;
    };
    const auto matching = run("measurement-match");
    const auto mismatched = run("measurement-mismatch");
    return runTrials(store, *t3, model, matching, mismatched, slug, commit);
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2) {
            const std::string_view option = argv[1];
            if (option == "--help") {
                usage(std::cout);
                return 0;
            }
            if (option == "--self-test") {
                selfTest();
                return 0;
            }
            if (option == "--live") return runLive();
        }
        if (argc != 7) {
            usage(std::cerr);
            return 2;
        }

        const auto matching = taskId(argv[3]);
        const auto mismatched = taskId(argv[4]);
        const std::string_view commit = argv[6];
        if (!validCommit(commit)) throw Conflict("The measurement commit must be a full Git commit SHA");
        const auto credentials = std::filesystem::path(argv[1]) / "credentials";
        if (!std::filesystem::exists(argv[2]))
            return live_test::skip("The fixture database is missing. Use --live to create both tasks automatically.", setup);
        if (!live_test::hasT3Credentials(credentials / "t3")) return live_test::skip("T3 credentials are missing.", setup);
        if (!live_test::hasChatGPTProfile(credentials / "chatgpt")) return live_test::skip("ChatGPT credentials are missing.", setup);
        app::ChatGPTModel model(credentials / "chatgpt");
        if (model.status().at("connected") == false) return live_test::skip("ChatGPT credentials are missing.", setup);
        auto t3 = makeT3Mcp(credentials / "t3");
        Store store(argv[2]);
        return runTrials(store, *t3, model, matching, mismatched, argv[5], commit);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
