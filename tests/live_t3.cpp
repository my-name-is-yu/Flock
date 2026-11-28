#include "flock/task_control.hpp"
#include "support.hpp"
#include "flock/task_history.hpp"
#include "flock/tasks.hpp"
#include "flock/person.hpp"
#include "flock/t3_mcp.hpp"
#include "live_setup.hpp"
#include "live_t3_fixture.hpp"
#include "flock/command.hpp"
#include "flock/json.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/t3_session.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"
#include "flock/task_target.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <sys/resource.h>

using namespace flockv0;
using namespace flockv0::live_test;

namespace {
class CrashOnAcknowledgement final : public T3 {
public:
    CrashOnAcknowledgement(T3& adapter, std::string command) : inner(adapter), tool(std::move(command)) {}
    std::string authGeneration(const std::string& env, const Json& config) override { return inner.authGeneration(env, config); }
    std::optional<Delivery> preflight(const Command& cmd) override { return inner.preflight(cmd); }
    Delivery send(const Command& cmd) override {
        const std::string name(commandToolName(cmd.tool));
        if ("before:" + name == tool) {
            std::cout << "Stopped before external send for " << name << " " << cmd.actionId << std::endl;
            std::_Exit(86);
        }
        auto result = inner.send(cmd);
        if ("defer:" + name == tool && result.kind == Delivery::Kind::Accepted) {
            result.kind = Delivery::Kind::Unknown;
            result.run.runId.clear();
        }
        if (name == tool && (result.kind == Delivery::Kind::Accepted || result.kind == Delivery::Kind::Unknown)) {
            std::cout << "Stopped after external send for " << name << " " << cmd.actionId << std::endl;
            std::_Exit(86);
        }
        return result;
    }
    Delivery reconcile(const Command& cmd) override { return inner.reconcile(cmd); }
    Observation observe(const std::string& env, const Json& config, const RunRef& run) override { return inner.observe(env, config, run); }
    InterruptOutcome interrupt(const std::string& env, const Json& config, const RunRef& run) override {
        return inner.interrupt(env, config, run);
    }
    Excerpt read(const ReadRequest& req) override { return inner.read(req); }
private:
    T3& inner;
    const std::string tool;
};

void usage(std::ostream& out) {
    out << "Usage:\n"
        << "  flock_live_t3 --live\n"
        << "  flock_live_t3 login <credentials-dir> <environment-id> <MCP URL>\n"
        << "  flock_live_t3 submit <db> <environment-id> <project-id> <case> [<provider-id> <model>]\n"
        << "  flock_live_t3 pump <db> <credentials-dir> <task-id> <target-state> [<crash-after-send-tool>]\n"
        << "  flock_live_t3 operate <db> <task-id> <wait-id> <operation> [<JSON>]\n"
        << "  flock_live_t3 show <db> <task-id>\n"
        << "  flock_live_t3 inspect <db> <credentials-dir> <task-id>\n"
        << "  flock_live_t3 profile <db> <credentials-dir> <task-id> <wait|read>\n\n"
        << "--live launches one smoke Run and checks its exact report. It creates a temporary database and uses the project's default model.\n"
        << t3Setup << "\n"
        << "For manual cases, use a scratch SQLite path for <db>. The summary and delegated-result cases need notes.md with pending items\n"
        << "for Mina and Alex. Measurement cases need executable measure_on.sh and measure_off.sh at the project root.\n"
        << "Set FLOCK_ACCEPTANCE_RUNTIME_MODE to override the default T3 runtime mode (auto).\n"
        << "Cases:";
    for (const auto scenario : t3Scenarios) out << ' ' << scenario;
    out << '\n';
}

int runSmoke() {
    const auto home = environment("FLOCK_HOME");
    const auto environmentId = environment("FLOCK_LIVE_T3_ENVIRONMENT");
    const auto projectId = environment("FLOCK_LIVE_T3_PROJECT");
    if (home.empty()) return skip("FLOCK_HOME is unset", t3Setup);
    if (environmentId.empty()) return skip("FLOCK_LIVE_T3_ENVIRONMENT is unset", t3Setup);
    if (projectId.empty()) return skip("FLOCK_LIVE_T3_PROJECT is unset", t3Setup);
    const auto directory = std::filesystem::path(home) / "credentials" / "t3";
    if (!hasT3Credentials(directory)) return skip("No saved T3 credentials in FLOCK_HOME", t3Setup);

    auto t3 = makeT3Mcp(directory);
    t3->authGeneration(environmentId, Json::object());
    flockv0::test::TemporaryDirectory scratch;
    Store store(scratch.path / "state.db");
    const auto id = submit(store, environmentId, projectId, "smoke");
    if (const auto status = pump(store, *t3, id, "completed"); status != 0) return status;
    for (const auto& item : TaskHistory(store).history(id)) {
        if (item.at("kind") != "completed") continue;
        const auto& report = item.at("data").at("text");
        if (report != smokeReport) throw Conflict("T3 smoke expected " + Json(smokeReport).dump() + ", got " + report.dump());
        return 0;
    }
    throw Conflict("T3 smoke did not save a completion report");
}

int profile(Store& store, T3& t3, Id id, std::string_view mode) {
    const auto task = taskJson(Tasks(store).task(id));
    if (task.at("activeRun").is_null()) throw Conflict("Profile fixture is not assigned to an active Run");
    std::size_t count = 0, bytes = 0, largest = 0;
    unsigned modelCalls = 0;
    if (mode == "wait") {
        TestModel model;
        Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; });
        for (int i = 0; i < 5; ++i) runtime.tick();
        modelCalls = model.calls;
        if (modelCalls || Tasks(store).task(id).task.state != TaskState::running)
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
            largest = std::max(largest, page.text.size());
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
    } else
        throw Conflict("Profile mode must be wait or read");
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw Conflict("Could not measure process RSS");
#ifdef __APPLE__
    const auto rssBytes = usage.ru_maxrss;
#else
    const auto rssBytes = usage.ru_maxrss * 1024;
#endif
    std::cout << Json{{"mode", mode},
                      {"task", task.at("request")},
                      {"peakRssBytes", rssBytes},
                      {"modelCalls", modelCalls},
                      {"historyCalls", count + (mode == "read" && count < 24)},
                      {"historyBytes", bytes},
                      {"largestExcerpt", largest}}
                     .dump()
              << '\n';
    return 0;
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage(std::cerr);
            return 2;
        }
        const std::string operation = argv[1];
        if (operation == "--help" && argc == 2) {
            usage(std::cout);
        } else if (operation == "--live" && argc == 2) {
            return runSmoke();
        } else if (operation == "login" && argc == 5) {
            loginT3(argv[2], argv[3], argv[4], [](const std::string& url) { std::cout << url << std::endl; });
            std::cout << "T3 login succeeded\n";
        } else if (operation == "submit" && (argc == 6 || argc == 8)) {
            Store store(argv[2]);
            const auto id =
                argc == 8 ? submit(store, argv[3], argv[4], argv[5], argv[6], argv[7]) : submit(store, argv[3], argv[4], argv[5]);
            std::cout << id << '\n';
        } else if (operation == "show" && argc == 4) {
            Store store(argv[2]);
            const auto id = std::stoll(argv[3]);
            std::cout << Json{{"task", taskJson(Tasks(store).task(id))}, {"history", TaskHistory(store).history(id, 0, 200)}}.dump(2)
                      << '\n';
        } else if (operation == "operate" && (argc == 6 || argc == 7)) {
            Store store(argv[2]);
            TaskControl(store).operate(localOwner, TaskTarget::from(Tasks(store).task(std::stoll(argv[3])).task, std::stoll(argv[4])),
                                       argv[5], argc == 7 ? Json::parse(argv[6]) : Json::object());
        } else if (operation == "inspect" && argc == 5) {
            Store store(argv[2]);
            auto t3 = makeT3Mcp(argv[3]);
            const auto task = taskJson(Tasks(store).task(std::stoll(argv[4])));
            const auto& saved = task.at("activeRun").is_null() ? task.at("lastRun") : task.at("activeRun");
            const RunRef ref{saved.at("threadId"), saved.at("runId"), saved.at("link")};
            const auto seen = t3->observe(task.at("environmentId"), task.at("config"), ref);
            std::cout << Json{{"settled", seen.kind == Observation::Kind::Finished},
                              {"assignedRun", ref.runId},
                              {"resultRun", seen.resultRunId},
                              {"status", seen.status},
                              {"report", seen.report}}
                             .dump()
                      << '\n';
        } else if (operation == "profile" && argc == 6) {
            Store store(argv[2]);
            auto t3 = makeT3Mcp(argv[3]);
            return profile(store, *t3, std::stoll(argv[4]), argv[5]);
        } else if (operation == "pump" && (argc == 6 || argc == 7)) {
            Store store(argv[2]);
            auto real = makeT3Mcp(argv[3]);
            CrashOnAcknowledgement adapter(*real, argc == 7 ? argv[6] : "");
            return pump(store, adapter, std::stoll(argv[4]), argv[5]);
        } else {
            usage(std::cerr);
            return 2;
        }
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
