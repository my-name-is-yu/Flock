#include "flock/policy.hpp"
#include "flock/t3_mcp.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sys/resource.h>
#include <thread>

using namespace flockv0;
using namespace std::chrono_literals;

namespace {
constexpr auto sameQuestion =
    "Use the user-input tool to ask exactly one question with id target_branch, header Branch, "
    "question 'Which branch?', options main with description 'Use main.' and dev with description 'Use dev.'. "
    "Do not rephrase any fields. Wait for the answer and then report the chosen branch. Do not edit files.";

class TestModel final : public Model {
public:
    unsigned calls = 0;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        ++calls;
        const auto request = input.task.at("request").get<std::string>();
        if (input.event.at("kind") == "intake") {
            std::string instruction = "Read notes.md in this fixture project. Report the two pending items with their owners. Keep the repository unchanged.";
            if (request == "interrupt") {
                instruction = "Start the harmless terminal command sleep 60 and wait for it to finish. Do not edit any files.";
            }
            if (request == "question") {
                instruction = "Use the available user-input tool to ask which of main or dev is the target branch. "
                              "Wait for that answer in the same Run. Then report the chosen branch. Do not edit any files.";
            }
            if (request == "ambiguous-question") instruction = sameQuestion;
            if (request == "approval") {
                instruction = "Use the terminal tool to execute `printf 'approval-fixture\\n'`, with "
                    "sandbox_permissions=require_escalated and justification 'Verify the Flock approval hold'. "
                    "Wait for approval if one is requested. Do not edit files.";
            }
            if (request == "delegated-result") {
                instruction = "Start an asynchronous T3 delegate using the delegate_task tool, asking it to read notes.md "
                              "and identify the two pending owners and their items. Do not write files. "
                              "After launching it, end this turn with 'child started'. When T3 wakes you with completion, "
                              "write the delegate's final list. Do not launch a second delegate.";
            }
            if (request.starts_with("measurement-")) {
                const auto flags = request == "measurement-mismatch" ? "off" : "on";
                instruction = std::string("Execute exactly the shell command `./measure_") + flags + ".sh`. Do not edit any project file. "
                    "Leave the measurement output in the terminal activity. The entire final answer must be exactly Measurement complete, with no punctuation or details.";
            }
            if (request == "profile-short" || request == "profile-long") {
                instruction = request == "profile-short"
                    ? "Run `printf 'short-history-fixture\\n'` in a terminal."
                    : "Run `seq 1 25000` in a terminal and request max_output_tokens=16000.";
                instruction += " Then execute `sleep 45` in a separate terminal tool call and wait for it to finish. Do not edit files.";
            }
            return {"delegate",{{"instruction",instruction},{"doneWhen","The fixture task and its result are documented"}},
                    "Send the fixed acceptance case to the isolated fixture project."};
        }
        if (input.event.at("kind") == "question") {
            auto answers = Json::object();
            const auto& question = input.event.at("question");
            for (const auto& item : question.at("fields")) {
                auto& answer = answers[item.at("id").get<std::string>()];
                answer = "main";
                for (const auto& option : item.value("options",Json::array())) {
                    if (option.is_object() && option.value("value",std::string{}).starts_with("main")) {
                        answer = option["value"];
                        break;
                    }
                }
            }
            return {"answer_session_question",{{"requestId",question.at("requestId")},{"answers",answers}},
                    "The fixture targets main."};
        }
        const auto report = input.event.at("report").get<std::string>();
        if (input.event.at("status") != "completed" || report.empty())
            return {"ask_user",{{"question","The fixture Run did not produce a successful report. Check T3."}},
                    "The Run status and report do not provide the required evidence."};
        if (request == "summary-follow" && input.task.at("additionalUsed") == 0)
            return {"follow_up",{{"instruction","Check that you named both owners from notes.md, and repeat the two pending items. Do not modify any files."}},
                    "Confirm the complete list within the same Thread."};
        if (request == "measurement-read-resume") {
            if (input.evidence.empty()) {
                return {"session_read",{{"threadId",input.event.at("run").at("threadId")},
                    {"runId",input.event.at("resultRunId")},{"view","activity"}},"Read the live measurement activity."};
            }
            if (input.evidence.size() != 1 || input.evidence[0].at("text").get<std::string>().empty())
                throw Conflict("The saved live excerpt was not restored");
        }
        if (request == "ambiguous-question" && input.task.at("additionalUsed") == 0)
            return {"follow_up",{{"instruction",sameQuestion}},"Repeat the exact same question on a separate Run."};
        if ((request.starts_with("summary") || request == "delegated-result") &&
            (report.find("Mina") == std::string::npos || report.find("Alex") == std::string::npos))
            return {"ask_user",{{"question","The report omitted an owner. Check the fixture output."}},
                    "The fixture requires both names."};
        return {"complete",{{"report",report}},"The report from the assigned T3 Run meets the fixture criteria."};
    }
};

class CrashOnAcknowledgement final : public T3 {
public:
    CrashOnAcknowledgement(T3& adapter, std::string command) : inner(adapter), tool(std::move(command)) {}
    std::string authGeneration(const std::string& env, const Json& config) override {
        return inner.authGeneration(env,config);
    }
    std::optional<Delivery> preflight(const Command& cmd) override { return inner.preflight(cmd); }
    Delivery send(const Command& cmd) override {
        if ("before:" + cmd.tool == tool) {
            std::cout << "Stopped before external send for " << cmd.tool << " " << cmd.actionId << std::endl;
            std::_Exit(86);
        }
        auto result = inner.send(cmd);
        if ("defer:" + cmd.tool == tool && result.kind == Delivery::Kind::Accepted) {
            result.kind = Delivery::Kind::Unknown;
            result.run.runId.clear();
        }
        if (cmd.tool == tool && (result.kind == Delivery::Kind::Accepted || result.kind == Delivery::Kind::Unknown)) {
            std::cout << "Stopped after external send for " << cmd.tool << " " << cmd.actionId << std::endl;
            std::_Exit(86);
        }
        return result;
    }
    Delivery reconcile(const Command& cmd) override { return inner.reconcile(cmd); }
    Observation observe(const std::string& env, const Json& config, const RunRef& run) override {
        return inner.observe(env,config,run);
    }
    InterruptOutcome interrupt(const std::string& env, const Json& config, const RunRef& run) override {
        return inner.interrupt(env,config,run);
    }
    Excerpt read(const ReadRequest& req) override { return inner.read(req); }
private:
    T3& inner;
    const std::string tool;
};

void usage() {
    std::cerr << "Usage:\n"
              << "  flock_live_t3 login <credentials-dir> <environment-id> <MCP URL>\n"
              << "  flock_live_t3 submit <db> <environment-id> <project-id> <case> [<provider-id> <model>]\n"
              << "  flock_live_t3 pump <db> <credentials-dir> <task-id> <target-state> [<crash-after-send-tool>]\n"
              << "  flock_live_t3 operate <db> <task-id> <wait-id> <operation> [<JSON>]\n"
              << "  flock_live_t3 show <db> <task-id>\n"
              << "  flock_live_t3 inspect <db> <credentials-dir> <task-id>\n"
              << "  flock_live_t3 profile <db> <credentials-dir> <task-id> <wait|read>\n";
}

int profile(Store& store,T3& t3, Id id, std::string_view mode) {
    const auto task = store.task(id);
    if (task.at("activeRun").is_null()) throw Conflict("Profile fixture is not assigned to an active Run");
    std::size_t count = 0, bytes = 0, largest = 0;
    unsigned modelCalls = 0;
    if (mode == "wait") {
        TestModel model;
        Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
        for (int i = 0; i < 5; ++i) runtime.tick();
        modelCalls = model.calls;
        if (modelCalls || store.task(id).at("state") != "running")
            throw Conflict("The benchmark Run did not remain in ordinary monitoring");
    } else if (mode == "read") {
        ReadRequest req;
        req.environmentId = task.at("environmentId");
        req.threadId = task.at("activeRun").at("threadId");
        req.runId = task.at("activeRun").at("runId");
        req.view = "activity";
        req.config = task.at("config");
        for (; count < 24; ++count) {
            const auto page = t3.read(req);
            if (page.text.size() > 8192) throw Conflict("A history excerpt exceeded 8 KiB");
            bytes += page.text.size();
            largest = std::max(largest,page.text.size());
            const bool advanced = req.itemId.empty() && !page.nextCursor.empty() && req.cursor != page.nextCursor;
            if (advanced) req.cursor = page.nextCursor;
            if (page.nextOffset && req.offset < page.nextOffset) {
                req.offset = page.nextOffset;
                req.itemId = page.itemId;
            } else if (!req.itemId.empty()) {
                req.offset = 0;
                req.itemId.clear();
            } else {
                if (!page.more || !advanced) break;
            }
        }
    } else throw Conflict("Profile mode must be wait or read");
    rusage usage{};
    if (getrusage(RUSAGE_SELF,&usage) != 0) throw Conflict("Could not measure process RSS");
#ifdef __APPLE__
    const auto rssBytes = usage.ru_maxrss;
#else
    const auto rssBytes = usage.ru_maxrss * 1024;
#endif
    std::cout << Json{{"mode",mode},{"task",task.at("request")},{"peakRssBytes",rssBytes},
        {"modelCalls",modelCalls},{"historyCalls",count+(mode=="read" && count<24)},{"historyBytes",bytes},
        {"largestExcerpt",largest}}.dump() << '\n';
    return 0;
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) { usage(); return 2; }
        const std::string operation = argv[1];
        if (operation == "login" && argc == 5) {
            loginT3(argv[2],argv[3],argv[4],[](const std::string& url) {
                std::cout << url << std::endl;
            });
            std::cout << "T3 login succeeded\n";
        } else if (operation == "submit" && (argc == 6 || argc == 8)) {
            Store store(argv[2]);
            const std::string request = argv[5];
            if (request != "summary" && request != "summary-follow" && request != "interrupt" &&
                request != "question" && request != "measurement-match" && request != "measurement-mismatch" &&
                request != "measurement-read-resume" &&
                request != "profile-short" && request != "profile-long" && request != "ambiguous-question" &&
                request != "approval" && request != "delegated-result") {
                throw std::runtime_error("Unknown fixture case");
            }
            Json config = {{"runTimeoutSeconds",300},{"session",{{"runtimeMode","auto"}}}};
            if (argc == 8) {
                config["session"]["providerInstanceId"] = argv[6];
                config["session"]["model"] = argv[7];
            }
            if (request == "question" || request == "ambiguous-question") config["session"]["interactionMode"] = "plan";
            if (const auto* mode = std::getenv("FLOCK_ACCEPTANCE_RUNTIME_MODE")) config["session"]["runtimeMode"] = mode;
            std::cout << store.submit({"acceptance",request,argv[3],argv[4],config}) << '\n';
        } else if (operation == "show" && argc == 4) {
            Store store(argv[2]);
            const auto id = std::stoll(argv[3]);
            std::cout << Json{{"task",store.task(id)},{"history",store.history(id,0,200)}}.dump(2) << '\n';
        } else if (operation == "operate" && (argc == 6 || argc == 7)) {
            Store store(argv[2]);
            store.operate(localOwner,TaskTarget::from(store.task(std::stoll(argv[3])),std::stoll(argv[4])),argv[5],argc == 7 ? Json::parse(argv[6]) : Json::object());
        } else if (operation == "inspect" && argc == 5) {
            Store store(argv[2]);
            auto t3 = makeT3Mcp(argv[3]);
            const auto task = store.task(std::stoll(argv[4]));
            const auto& saved = task.at("activeRun").is_null() ? task.at("lastRun") : task.at("activeRun");
            const RunRef ref{saved.at("threadId"),saved.at("runId"),saved.at("link")};
            const auto seen = t3->observe(task.at("environmentId"),task.at("config"),ref);
            std::cout << Json{{"settled",seen.settled},{"assignedRun",ref.runId},{"resultRun",seen.resultRunId},
                {"status",seen.status},{"report",seen.report}}.dump() << '\n';
        } else if (operation == "profile" && argc == 6) {
            Store store(argv[2]);
            auto t3 = makeT3Mcp(argv[3]);
            return profile(store,*t3,std::stoll(argv[4]),argv[5]);
        } else if (operation == "pump" && (argc == 6 || argc == 7)) {
            Store store(argv[2]);
            auto real = makeT3Mcp(argv[3]);
            CrashOnAcknowledgement adapter(*real,argc == 7 ? argv[6] : "");
            TestModel model;
            const auto id = std::stoll(argv[4]);
            RuntimeOptions options;
            if (store.task(id).at("request") == "measurement-read-resume") options.sessionRead = "on_demand";
            Runtime runtime(store,model,adapter,[](std::string_view) { return std::string("Validate the acceptance fixture"); },options);
            std::string previous;
            const auto end = std::chrono::steady_clock::now() + 5min;
            do {
                const auto task = store.task(id);
                const auto state = task.at("state").get<std::string>();
                if (previous != state) {
                    std::cout << state << " ";
                    if (!task.at("activeRun").is_null()) std::cout << task.at("activeRun").dump();
                    if (!task.at("wait").is_null()) std::cout << task.at("wait").dump();
                    std::cout << std::endl;
                    previous = state;
                }
                if (state == argv[5]) return 0;
                if (std::string_view(argv[5]) == "read-saved") {
                    const auto history = store.history(id);
                    if (std::any_of(history.begin(),history.end(),[](const Json& item) { return item["kind"] == "session_read"; }))
                        return 0;
                }
                if ((state == "waiting" && task.at("wait").value("reason","") != "t3_input") ||
                    state == "completed" || state == "cancelled") return 3;
                runtime.tick();
                std::this_thread::sleep_for(250ms);
            } while (std::chrono::steady_clock::now() < end);
            std::cerr << "Live fixture timed out\n";
            return 4;
        } else {
            usage();
            return 2;
        }
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
