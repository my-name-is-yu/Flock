#include "flock/core.hpp"
#include "flock/t3_mcp.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace flockv0;
using namespace std::chrono_literals;

namespace {
class TestModel final : public Model {
public:
    Decision decide(const ModelInput& input) override {
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
            return {"delegate",{{"instruction",instruction},{"doneWhen","The fixture task and its result are documented"}},
                    "Send the fixed acceptance case to the isolated fixture project."};
        }
        if (input.event.at("kind") == "question") {
            auto answers = Json::object();
            const auto& question = input.event.at("question");
            for (const auto& item : question.at("fields"))
                answers[item.at("id").get<std::string>()] = "main";
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
        if (request.starts_with("summary") && (report.find("Mina") == std::string::npos || report.find("Alex") == std::string::npos))
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
        auto result = inner.send(cmd);
        if (cmd.tool == tool && (result.kind == Delivery::Kind::Accepted || result.kind == Delivery::Kind::Unknown)) {
            std::cout << "Stopped after external send for " << cmd.tool << " " << cmd.actionId << std::endl;
            // Exercise the durable send-start and reconciliation paths with no
            // application response saved and no RAII cleanup.
            std::_Exit(86);
        }
        return result;
    }
    Delivery reconcile(const Command& cmd) override { return inner.reconcile(cmd); }
    Observation observe(const std::string& env, const Json& config, const RunRef& run) override {
        return inner.observe(env,config,run);
    }
    void interrupt(const std::string& env, const Json& config, const RunRef& run) override {
        inner.interrupt(env,config,run);
    }
    Excerpt read(const ReadRequest& req) override { return inner.read(req); }
private:
    T3& inner;
    const std::string tool;
};

void usage() {
    std::cerr << "Usage:\n"
              << "  flock_live_t3 login <credentials-dir> <environment-id> <MCP URL>\n"
              << "  flock_live_t3 submit <db> <environment-id> <project-id> <summary|summary-follow|question|interrupt> [<provider-id> <model>]\n"
              << "  flock_live_t3 pump <db> <credentials-dir> <task-id> <target-state> [<crash-after-send-tool>]\n"
              << "  flock_live_t3 operate <db> <task-id> <wait-id> <operation> [<JSON>]\n"
              << "  flock_live_t3 show <db> <task-id>\n";
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
            if (request != "summary" && request != "summary-follow" && request != "interrupt" && request != "question") {
                throw std::runtime_error("Unknown fixture case");
            }
            Json config = {{"runTimeoutSeconds",300},{"session",{{"runtimeMode","auto"}}}};
            if (argc == 8) {
                config["session"]["providerInstanceId"] = argv[6];
                config["session"]["model"] = argv[7];
            }
            if (request == "question") config["session"]["interactionMode"] = "plan";
            std::cout << store.submit({"acceptance",request,argv[3],argv[4],config}) << '\n';
        } else if (operation == "show" && argc == 4) {
            Store store(argv[2]);
            const auto id = std::stoll(argv[3]);
            std::cout << Json{{"task",store.task(id)},{"history",store.history(id,0,200)}}.dump(2) << '\n';
        } else if (operation == "operate" && (argc == 6 || argc == 7)) {
            Store store(argv[2]);
            store.operate(std::stoll(argv[3]),std::stoll(argv[4]),argv[5],argc == 7 ? Json::parse(argv[6]) : Json::object());
        } else if (operation == "pump" && (argc == 6 || argc == 7)) {
            Store store(argv[2]);
            auto real = makeT3Mcp(argv[3]);
            CrashOnAcknowledgement adapter(*real,argc == 7 ? argv[6] : "");
            TestModel model;
            Runtime runtime(store,model,adapter,[](std::string_view) { return BotNotes{"Validate the acceptance fixture",""}; });
            const auto id = std::stoll(argv[4]);
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
                if (state == "waiting" || state == "completed" || state == "cancelled") return 3;
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
