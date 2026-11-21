#include "flock/core.hpp"
#include "flock/t3_mcp.hpp"
#include "model.hpp"

#include <iostream>

using namespace flockv0;

namespace {
struct Recording {
    Json event, config;
    Excerpt evidence;
};

Recording load(Store& store, T3& t3, Id id, std::string_view commit, bool matches) {
    const auto task = store.task(id);
    Json report;
    for (const auto& item : store.history(id)) {
        if (item["kind"] == "report") report = item["data"];
    }
    if (report.is_null() || report.at("report") != "計測済み")
        throw Conflict("Both fixtures must have exactly the same final report");
    const auto& run = report.at("run");
    ReadRequest request;
    request.environmentId = task.at("environmentId");
    request.threadId = run.at("threadId");
    request.runId = report.at("resultRunId");
    request.view = "activity";
    request.config = task.at("config");
    const auto expected = "FLOCK_MEASUREMENT commit=" + std::string(commit) + " updates=" + (matches ? "on" : "off");
    for (int page = 0; page < 30; ++page) {
        const auto excerpt = t3.read(request);
        if (excerpt.text.find(expected) != std::string::npos && excerpt.text.find("peak_rss=") != std::string::npos) {
            report["buildOutput"] = {{"environmentId",request.environmentId},{"threadId",request.threadId},
                                    {"runId",request.runId},{"itemId",excerpt.itemId},{"view","activity"}};
            request.cursor.clear();
            request.itemId = excerpt.itemId;
            const auto exact = t3.read(request);
            if (exact.text != excerpt.text) throw Conflict("The chosen T3 item changed");
            return {report,request.config,exact};
        }
        if (!excerpt.more || excerpt.nextCursor.empty() || request.cursor == excerpt.nextCursor) break;
        request.cursor = excerpt.nextCursor;
    }
    throw Conflict("The live T3 activity did not contain the expected measurement");
}

Json evidenceOf(const Recording& recording) {
    auto result = recording.event.at("buildOutput");
    result["text"] = recording.evidence.text;
    result["more"] = recording.evidence.more;
    return result;
}

void evaluate(T3& t3, Model& model, const Recording& recording, std::string_view commit,
              std::string_view slug, std::string_view mode, bool matches) {
    const auto criterion = "At commit " + std::string(commit) +
        ", measure peak RSS for the build with updates enabled and report the numeric result.";
    ModelInput input {{ {"id",1},{"botId","memory"},{"request",criterion},{"doneWhen",criterion},
                        {"remaining",3},{"additionalUsed",0} },
                      recording.event,Json::array(),
                      {"Validate the measured commit, setting and numeric RSS with evidence. "
                       "The generic final report alone cannot establish the criteria. "
                       "Use a supplied build-output item as evidence. If it has not been supplied and session_read is available, "
                       "read that item. If evidence remains insufficient, follow up for the missing evidence.",
                       "buildOutput identifies the terminal activity for the measurement."},
                      {"follow_up","complete","ask_user"},"",Json::array(),
                      {{"modelProvider","chatgpt"},{"model",slug}}};
    if (mode == "on_demand") input.tools.push_back("session_read");
    if (mode == "prefill") input.evidence.push_back(evidenceOf(recording));
    Json trials = Json::array();
    Decision chosen;
    for (int turn = 0; turn < 4; ++turn) {
        chosen = model.decide(input);
        trials.push_back({{"tool",chosen.tool},{"reason",chosen.reason},{"usage",chosen.usage},
                          {"inputBytes",(input.task.dump()+input.event.dump()+input.evidence.dump()).size()}});
        if (chosen.tool != "session_read") break;
        const auto& locator = recording.event.at("buildOutput");
        if (chosen.args.at("threadId") != locator.at("threadId") || chosen.args.at("runId") != locator.at("runId") ||
            chosen.args.value("itemId",locator.at("itemId")) != locator.at("itemId"))
            throw Conflict("The model selected evidence outside the recorded Run and item");
        ReadRequest request;
        request.environmentId = locator.at("environmentId");
        request.threadId = locator.at("threadId");
        request.runId = locator.at("runId");
        request.itemId = locator.at("itemId");
        request.view = "activity";
        request.config = recording.config;
        auto fetched = t3.read(request);
        if (fetched.text != recording.evidence.text) throw Conflict("T3 returned different evidence on demand");
        input.evidence.push_back(evidenceOf(recording));
        if (input.evidence.size() == 3) input.tools.pop_back();
    }
    const auto falseCompletion = !matches && chosen.tool == "complete";
    const auto unnecessaryWork = matches && chosen.tool == "follow_up";
    Json result = {{"mode",mode},{"matchingEvidence",matches},{"action",chosen.tool},{"args",chosen.args},
        {"falseCompletion",falseCompletion},{"unnecessaryWork",unnecessaryWork},{"turns",trials}};
    std::cout << result.dump() << std::endl;
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 7) throw Conflict("Usage: flock_live_evidence <home> <db> <matchingTaskId> <mismatchedTaskId> <model> <commit>");
        const std::filesystem::path credentials = std::filesystem::path(argv[1]) / "credentials";
        auto t3 = makeT3Mcp(credentials / "t3");
        flockv0::app::ChatGPTModel model(credentials / "chatgpt");
        Store store(argv[2]);
        for (const bool matches : {true,false}) {
            const auto recording = load(store,*t3,std::stoll(argv[matches ? 3 : 4]),argv[6],matches);
            for (const std::string_view mode : {"off","on_demand","prefill"})
                evaluate(*t3,model,recording,argv[6],argv[5],mode,matches);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
