#include "flock/conversations.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/board_inbox.hpp"
#include "flock/workspace_tools.hpp"
#include "bot_tools.hpp"
#include "model.hpp"
#include "../src/workspace_internal.hpp"
#include "flock/decision.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/session_ref.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/t3_session.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace flockv0;
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

const Person owner{"owner", true};
constexpr std::string_view dbMarker = "state-only-marker";
constexpr std::string_view secretMarker = "vault-only-marker";
constexpr std::string_view peerMarker = "other-bot-only-marker";
constexpr std::string_view historyMarker = "session-only-marker";

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
void write(const fs::path& path, std::string_view data) {
    std::ofstream file(path);
    file << data;
    check(file.good(), "Could not write fixture data");
}
std::string read(const fs::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}
Decision call(std::string tool, Json args = Json::object()) {
    return {std::move(tool), std::move(args), {}};
}

struct Provider : Model {
    std::vector<Decision> next;
    Json seen;
    Json content;
    std::function<void(const ModelInput&)> beforeTurn;
    std::function<void(const Json&)> inspectRequest;
    std::function<std::vector<Decision>(const Json&)> followStep;
    ModelTurn decide(const ModelInput& input) override {
        auto request = app::responseRequest(input);
        if (inspectRequest) inspectRequest(request);
        auto output = Json::array();
        auto emit = [&](const Decision& action) {
            auto args = action.args;
            if (!action.reason.empty()) args["reason"] = action.reason;
            output.push_back({{"type", "function_call"},
                              {"namespace", "flock"},
                              {"status", "completed"},
                              {"name", action.tool},
                              {"arguments", args.dump()}});
        };
        if (input.event.at("kind") != "conversation_turn") {
            emit({"delegate",
                  {{"instruction", "Read private history"}, {"doneWhen", "Private fixture checked"}},
                  "The evidence is in the Session."});
        } else {
            if (beforeTurn) beforeTurn(input);
            check(request.at("instructions").get<std::string>().find("untrustedData") != std::string::npos,
                  "Tool output trust was not explained in the prompt");
            const auto& steps = input.event.at("turn");
            if (steps.empty()) {
                for (const auto& action : next) emit(action);
            } else {
                seen = steps;
                content = request.at("input").at(0).at("content");
                auto follow = followStep ? followStep(steps) : std::vector<Decision>{};
                if (follow.empty())
                    emit(call("reply", {{"text", "Checked."}}));
                else
                    for (const auto& action : follow) emit(action);
            }
        }
        return app::responseDecision(output, request);
    }
};

struct LocalT3 : T3 {
    bool launched = false;
    std::string authGeneration(const std::string&, const Json&) override { return "connected"; }
    Delivery send(const Command&) override {
        launched = true;
        return {Delivery::Kind::Accepted, {"private-thread", "private-run", {}}, {}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef&) override { return {}; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { return InterruptOutcome::Requested; }
    std::optional<SessionMetadata> sessionMetadata(const SessionTarget&, bool, SessionReadDeadline) override {
        return SessionMetadata{std::string(historyMarker),
                               "private-project",
                               "private-project",
                               "running",
                               "2026-11-21T00:00:00Z",
                               "2026-11-21T00:00:00Z",
                               {},
                               false};
    }
};

struct FakeWeb : WebTools {
    std::atomic<int> searches{0}, fetches{0};
    bool synchronize = false;
    std::mutex mutex;
    std::condition_variable gate;
    std::vector<std::string> searchQueries;
    unsigned arrived = 0;
    void rendezvous() {
        if (!synchronize) return;
        std::unique_lock lock(mutex);
        ++arrived;
        gate.notify_all();
        if (!gate.wait_for(lock, 2s, [&] { return arrived == 2; })) throw Conflict("Independent web calls did not overlap");
    }
    Json search(std::string_view query) override {
        ++searches;
        {
            std::lock_guard lock(mutex);
            searchQueries.emplace_back(query);
        }
        rendezvous();
        if (query == "complete links")
            return {{"results", Json::array({{{"title", std::string(250, 'x') + std::string(secretMarker)},
                                              {"url", "https://example.com/vault-"},
                                              {"description", "Evidence"}}})}};
        return {{"query", query}, {"text", "Untrusted search evidence"}};
    }
    Json fetch(std::string_view url) override {
        ++fetches;
        rendezvous();
        return {{"url", url},
                {"text", url.ends_with("/vault-") ? "Partial value: vault-" : "Untrusted page evidence"},
                {"truncated", url.ends_with("/vault-")}};
    }
};

struct FakeMcp : McpTools {
    std::atomic<int> discoveries{0}, calls{0}, reads{0};
    bool hasServer(std::string_view server) const override { return server == "fixture" || server == "private"; }
    Json discover(std::string_view, std::string_view, McpCollection collection, std::string_view cursor) override {
        ++discoveries;
        if (collection == McpCollection::resources) return {{"resources", {"fixture://data"}}};
        if (cursor == "tools-page-2") return {{"tools", {"second"}}};
        return {{"tools", {"lookup"}}, {"nextCursor", "tools-page-2"}};
    }
    McpToolResult invoke(std::string_view, std::string_view name, const Json& args) override {
        ++calls;
        return {{{"toolName", name}, {"arguments", args}}, false};
    }
    Json readResource(std::string_view, std::string_view uri) override {
        ++reads;
        return {{"uri", uri}};
    }
};

struct Fixture {
    fs::path home;
    std::unique_ptr<Store> store;
    Provider model;
    LocalT3 t3;
    std::shared_ptr<BotTools> tools = std::make_shared<BotTools>();
    std::shared_ptr<FakeWeb> web = std::make_shared<FakeWeb>();
    std::shared_ptr<FakeMcp> mcp = std::make_shared<FakeMcp>();
    std::unique_ptr<Runtime> runtime;
    unsigned seq = 0;
    Id lastMessage = 0;

    Fixture() {
        char pattern[] = "/tmp/flock-workspace-XXXXXX";
        const auto name = mkdtemp(pattern);
        check(name != nullptr, "Could not create test workspace");
        home = fs::canonical(name);
        tools->web = web;
        tools->mcp = mcp;
        restart();
    }
    ~Fixture() {
        runtime.reset();
        store.reset();
        fs::remove_all(home);
    }
    void restart() {
        runtime.reset();
        store = std::make_unique<Store>(home / "state.db");
        RuntimeOptions options;
        options.botTools = tools;
        runtime = std::make_unique<Runtime>(*store, model, t3, [](auto) { return std::string{}; }, options);
    }
    Intake intake(std::string bot = "alpha", Id limit = 20) const {
        return {std::move(bot),
                "",
                "test-environment",
                "private-project",
                {{"modelCallLimit", limit}, {"model", "test"}, {"modelProvider", "chatgpt"}}};
    }
    void block(Action action, std::string_view target, bool blocked = true) {
        store->policy().changeNeverRule(owner, action, target, blocked);
    }
    Json batch(std::vector<Decision> calls, Id budget = 20, std::string bot = "alpha") {
        model.next = std::move(calls);
        model.seen = nullptr;
        lastMessage = Conversations(*store)
                          .receive(owner, intake(std::move(bot), budget), std::to_string(++seq), "Check the fixture")
                          .at("messageId");
        runtime->runConversations();
        return model.seen;
    }
    Json run(Decision action, std::string bot = "alpha") {
        const auto steps = batch({std::move(action)}, 20, std::move(bot));
        check(steps.is_array() && steps.size() == 1, "Expected one tool result");
        return steps[0].at("result");
    }
    void approvePaused(const Json& pause) {
        const Id wait = pause.at("messageId");
        Inbox(*store).decideInbox(owner, {"alpha", InboxKind::conversation_model_budget, pause.at("replyToMessageId"), wait, 0, 0, wait},
                                  "approve");
    }
    void approve() {
        const auto paused = Conversations(*store).pausedConversations("alpha");
        check(paused.size() == 1, "Expected one paused turn");
        approvePaused(paused.at(0));
        runtime->runConversations();
    }
    std::string sql(const std::string& query) const {
        sqlite3* connection = nullptr;
        const auto opened = sqlite3_open_v2((home / "state.db").c_str(), &connection, SQLITE_OPEN_READONLY, nullptr);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(connection, sqlite3_close);
        check(opened == SQLITE_OK, "Fixture database open failed");
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(db.get(), query.c_str(), -1, &statement, nullptr) == SQLITE_OK, "Fixture SQL preparation failed");
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(statement, sqlite3_finalize);
        check(sqlite3_step(row.get()) == SQLITE_ROW, "Fixture SQL row missing");
        return reinterpret_cast<const char*>(sqlite3_column_text(row.get(), 0));
    }
};

void workspaceFiles(Fixture& f) {
    Json request;
    f.model.inspectRequest = [&](const Json& input) { request = input; };
    const auto normal = call("workspace_write_file", {{"path", "allowed.txt"}, {"text", "workspace-only-marker"}});
    f.block(Action::workspace_write, "alpha");
    check(!f.run(normal).at("ok").get<bool>(), "Workspace write bypassed a Never rule");
    f.model.inspectRequest = {};
    std::vector<std::string> workspaceTools;
    for (const auto& function : request.at("tools").at(0).at("tools")) {
        const auto& name = function.at("name").get_ref<const std::string&>();
        if (name.starts_with("workspace_")) workspaceTools.push_back(name);
    }
    std::ranges::sort(workspaceTools);
    check(workspaceTools == std::vector<std::string>{"workspace_read_file", "workspace_search", "workspace_write_file"},
          "The model must receive only file tools for the Bot workspace");
    f.block(Action::workspace_write, "alpha", false);
    check(f.run(normal).at("ok") == true, "Allowed workspace write failed");
    const auto root = f.home / "workspaces" / "alpha";
    const auto marker = f.run(call("workspace_search", {{"query", "workspace-only-marker"}}));
    check(marker.value("files", Json::array()) == Json::array({"allowed.txt"}), "Workspace search missed its marker: " + marker.dump());
    check(f.run(call("workspace_read_file", {{"path", "allowed.txt"}})).at("text") == "workspace-only-marker",
          "Workspace read missed its content");
    const auto messageId = std::to_string(f.lastMessage);
    check(f.sql("SELECT count(*) FROM conversation_model_failures WHERE message_id=" + messageId) == "0" &&
              f.sql("SELECT count(*) FROM conversation_steps, json_each(decision) WHERE message_id=" + messageId +
                    " AND json_type(json_each.value,'$.reason') IS NOT NULL") == "0",
          "Routine workspace read and reply required repair or persisted a rationale");
    write(root / "script", "#!/bin/sh\n");
    check(chmod((root / "script").c_str(), 0700) == 0, "Could not make fixture script executable");
    check(f.run(call("workspace_write_file", {{"path", "script"}, {"text", "#!/bin/sh\nprintf updated"}})).at("ok") == true &&
              access((root / "script").c_str(), X_OK) == 0,
          "Workspace edit lost executable permissions");
    const auto secret = f.store->policy().requestSecret("beta", {"fixture-key"});
    f.store->policy().submitSecret(owner, "beta", secret, secretMarker);
    const auto secretFile = fs::directory_iterator(f.home / "secrets")->path();
    fs::create_directories(f.home / "workspaces" / "beta");
    const auto peer = f.home / "workspaces" / "beta" / "private";
    write(peer, peerMarker);
    const auto history = f.home / "private-session.jsonl";
    write(history, historyMarker);
    auto privateIntake = f.intake("beta");
    privateIntake.request = dbMarker;
    f.store->submit(privateIntake);
    for (int i = 0; i < 12 && !f.t3.launched; ++i) f.runtime->tick();
    check(f.t3.launched, "Private T3 fixture did not start");
    const auto sessionScope = Resource::sessionRead("test-environment", "private-thread");
    const auto searchSession = call("flock_session_search");
    check(f.run(searchSession).at("sessions").size() == 1, "Session fixture did not appear after removing the Never rule");
    f.block(sessionScope.action, sessionScope.target);
    check(f.run(searchSession).at("sessions").empty(), "Revoked Session was still readable");
    f.sql("SELECT 1 FROM tasks WHERE data LIKE '%state-only-marker%'");
    check(read(secretFile) == secretMarker, "Secret marker was not persisted in the vault");

    const std::array<fs::path, 4> paths{f.home / "state.db", secretFile, peer, history};
    const std::array<std::string_view, 4> markers{dbMarker, secretMarker, peerMarker, historyMarker};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto& target = paths[i];
        check(f.run(call("workspace_read_file", {{"path", target.string()}})).at("ok") == false, "Absolute private file was readable");
        check(f.run(call("workspace_read_file", {{"path", "../../" + target.filename().string()}})).at("ok") == false,
              "Traversal was accepted");
        const auto alias = root / "escape";
        fs::create_symlink(target, alias);
        check(f.run(call("workspace_read_file", {{"path", "escape"}})).at("ok") == false, "Symlink read escaped");
        check(f.run(call("workspace_write_file", {{"path", "escape"}, {"text", "overwritten"}})).at("ok") == false,
              "Symlink write escaped");
        const auto result = f.run(call("workspace_search", {{"query", markers[i]}}));
        if (i == 1)
            check(result.value("ok", true) == false && result.dump().find(secretMarker) == std::string::npos,
                  "Protected query text entered a workspace search");
        else
            check(result.value("ok", false) && result.at("files").empty(), "Symlink search escaped or failed: " + result.dump());
        fs::remove(alias);
    }

    fs::create_hard_link(peer, root / "private-link");
    check(f.run(call("workspace_read_file", {{"path", "private-link"}})).at("ok") == false, "Hard link read escaped");
    check(f.run(call("workspace_write_file", {{"path", "private-link"}, {"text", "overwritten"}})).at("ok") == false,
          "Hard link write was accepted");
    check(f.run(call("workspace_search", {{"query", peerMarker}})).at("files").empty(), "Hard link search escaped");
}

void mountedWorkspaceDirectoryIdentity() {
#if defined(__linux__) || defined(__APPLE__)
#ifdef __linux__
    const auto* mount = "proc";
#else
    const auto* mount = "dev";
#endif
    const auto root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(root >= 0, "Could not open the root directory");
    int child;
    try {
        child = openExactChildDirectory(root, mount);
    } catch (...) {
        close(root);
        throw;
    }
    struct stat rootInfo{}, childInfo{};
    const bool distinct = fstat(root, &rootInfo) == 0 && fstat(child, &childInfo) == 0 && rootInfo.st_dev != childInfo.st_dev;
    close(child);
    close(root);
    check(distinct, "The test directory is not a separate mount");
#endif
}

void externalTools(Fixture& f) {
    const auto search = call("web_search", {{"query", "public fixture"}});
    const auto fetch = call("web_fetch", {{"url", "https://example.com/evidence"}});
    f.block(Action::web_search, "public");
    check(f.run(search).at("ok") == false && f.web->searches == 0, "Blocked web search reached its adapter");
    f.block(Action::web_search, "public", false);
    f.web->synchronize = true;
    const auto paired = f.batch({search, fetch});
    f.web->synchronize = false;
    check(paired[0].at("tool") == "web_search" && paired[1].at("tool") == "web_fetch", "Parallel calls returned out of order");
    for (const auto& step : paired)
        check(step.at("result").at("ok") == true && step.at("result").at("untrustedData") == true,
              "Concurrent web result was invalid or untagged");
    f.block(Action::web_fetch, "other.example");
    check(f.run(call("web_fetch", {{"url", "https://other.example/evidence"}})).at("ok") == false && f.web->fetches == 1,
          "Web fetch bypassed the host restriction");
    check(f.run(call("web_fetch", {{"url", "https://example.com@127.0.0.1/"}})).at("ok") == false,
          "An invalid or internal fetch target was accepted");
    const auto redacted = f.run(call("web_fetch", {{"url", "https://example.com/vault-"}})).at("data");
    check(redacted.at("url") == "https://example.com/vault-" && redacted.at("text") == "Partial value: [redacted]",
          "A complete URL and a truncated body did not receive different redaction");
    const auto link = f.run(call("web_search", {{"query", "complete links"}})).at("data").at("results").at(0);
    check(link.at("url") == "https://example.com/vault-" && link.at("title") == std::string(250, 'x') + "[redac",
          "Search did not preserve complete URLs and redact titles before bounding them");

    const auto discovery = call("mcp_discover", {{"server", "fixture"}, {"collection", "tools"}});
    const auto discover = f.run(discovery);
    const auto invoke = call("mcp_call_tool", {{"server", "fixture"}, {"name", "lookup"}, {"arguments", {{"query", "x"}}}});
    const auto resource = call("mcp_read_resource", {{"server", "fixture"}, {"uri", "fixture://data"}});
    check(discover.at("data").at("tools") == Json::array({"lookup"}) && discover.at("untrustedData") == true,
          "MCP discovery result was invalid");
    auto nextPage = discovery;
    nextPage.args["cursor"] = discover.at("data").at("nextCursor");
    check(f.run(nextPage).at("data").at("tools") == Json::array({"second"}), "MCP tool cursor did not reach the adapter");
    const auto invoked = f.run(invoke);
    check(invoked.at("data").at("toolName") == "lookup" && invoked.at("untrustedData") == true, "MCP invocation failed");
    const auto readResource = f.run(resource);
    check(readResource.at("data").at("uri") == "fixture://data" && readResource.at("untrustedData") == true, "MCP resource read failed");
    for (const auto* op : {"discover", "invoke", "read_resource"}) f.block(*parseAction(std::string("mcp.") + op), "fixture");
    check(f.run(discovery).at("ok") == false && f.mcp->discoveries == 2, "Revoked MCP discovery reached the adapter");
    check(f.run(invoke).at("ok") == false && f.mcp->calls == 1, "Revoked MCP invocation reached the adapter");
    check(f.run(resource).at("ok") == false && f.mcp->reads == 1, "Revoked MCP resource read reached the adapter");

    const auto skill = f.home / "fixture-skill";
    fs::create_directory(skill);
    write(skill / "SKILL.md", "Read this fixture skill.");
    fs::create_symlink(f.home / "state.db", skill / "escape");
    f.tools->skills.emplace("fixture", skill);
    f.block(Action::skill_read, "fixture");
    check(f.run(call("skill_list")).at("data").empty(), "Skill catalog leaked a denied skill");
    f.block(Action::skill_read, "fixture", false);
    check(f.run(call("skill_list")).at("data") == Json::array({{{"name", "fixture"}}}), "Skill catalog omitted an allowed skill");
    const auto instructions = f.run(call("skill_read", {{"skill", "fixture"}}));
    check(instructions.at("data").at("text") == "Read this fixture skill." && instructions.at("untrustedData") == true,
          "Skill read failed");
    check(f.run(call("skill_read", {{"skill", "fixture"}, {"path", "escape"}})).at("ok") == false, "Skill read traversed a symlink");
    check(f.run(call("skill_read", {{"skill", "fixture"}, {"path", "../state.db"}})).at("ok") == false, "Skill read traversed its root");

    const auto media =
        Json::array({{{"name", "pixel.png"},
                      {"mimeType", "image/png"},
                      {"dataBase64", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"}},
                     {{"name", "tone.wav"},
                      {"mimeType", "audio/wav"},
                      {"dataBase64", "UklGRiYAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQIAAACAgA=="}},
                     {{"name", "corrupt.png"}, {"mimeType", "image/png"}, {"dataBase64", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQ"}},
                     {{"name", secretMarker}, {"mimeType", "audio/mpeg"}, {"dataBase64", "SUQzdmF1bHQtb25seS1tYXJrZXI="}}});
    f.model.next = {call("reply", {{"text", "Message received."}})};
    const auto message =
        Conversations(*f.store).receive(owner, f.intake(), "media", "Please inspect these photos and audio", 0, media).at("messageId");
    f.block(Action::attachment_inspect, "alpha");
    f.runtime->runConversations();
    const auto image = call("inspect_attachment", {{"messageId", message}, {"attachmentId", "1"}});
    const auto audio = call("inspect_attachment", {{"messageId", message}, {"attachmentId", "2"}});
    check(f.run(image).at("ok") == false, "Attachment inspection bypassed the Never rule");
    check(f.model.content.size() == 1, "Blocked image reached the model request");
    f.block(Action::attachment_inspect, "alpha", false);
    const auto metadata = f.batch({image, audio});
    check(metadata[0].at("result").at("data").at("kind") == "image" && metadata[0].at("result").at("data").at("width") == 1 &&
              metadata[0].at("result").at("untrustedData") == true && metadata[1].at("result").at("data").at("kind") == "audio" &&
              metadata[1].at("result").at("data").at("sampleRateHz") == 8000 && metadata[1].at("result").at("untrustedData") == true,
          "Media inspection did not return scoped metadata");
    check(f.model.content.size() == 3 && f.model.content[2].at("type") == "input_image" &&
              f.model.content[2].at("image_url") == "data:image/png;base64," + media[0].at("dataBase64").get<std::string>(),
          "An authorized attachment did not reach the production image input schema");
    check(f.run(image, "beta").at("ok") == false, "Another Bot could inspect the attachment");
    const auto corrupt = f.run(call("inspect_attachment", {{"messageId", message}, {"attachmentId", "3"}}));
    check(corrupt.at("ok") == false && f.model.content.size() == 1, "A corrupt image reached the model or interrupted the turn");
    const auto opaque = f.run(call("inspect_attachment", {{"messageId", message}, {"attachmentId", "4"}}));
    check(opaque.at("ok") == true && opaque.at("data").at("name") == "[redacted]", "Attachment inspection exposed a shared secret value");
    check(Conversations(*f.store).receipt(owner, "alpha", "media", "Please inspect these photos and audio", 0, media).has_value(),
          "Attachment receipt was not idempotent");
    const auto jpeg =
        Json::array({{{"name", "pixel.jpg"},
                      {"mimeType", "image/jpeg"},
                      {"dataBase64", "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIx"
                                     "wcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAAAAAAAAAAAAAAAP/EABQQAQAAAAAA"
                                     "AAAAAAAAAAAAAAD/2gAIAQEAAD8AP//Z"}},
                     {{"name", "corrupt.jpg"}, {"mimeType", "image/jpeg"}, {"dataBase64", "/9j/4AAQSkZJRgABAQAAAQABAAA="}}});
    f.model.next = {call("reply", {{"text", "Message received."}})};
    const auto pictures = Conversations(*f.store).receive(owner, f.intake(), "jpeg", "Please inspect these JPEGs", 0, jpeg).at("messageId");
    f.runtime->runConversations();
    const auto jpegViews = f.batch({call("inspect_attachment", {{"messageId", pictures}, {"attachmentId", "1"}}),
                                    call("inspect_attachment", {{"messageId", pictures}, {"attachmentId", "2"}})});
    check(jpegViews[0].at("result").at("data").at("width") == 1 && jpegViews[1].at("result").at("ok") == false &&
              f.model.content.size() == 3 &&
              f.model.content.at(2).at("image_url") == "data:image/jpeg;base64," + jpeg[0].at("dataBase64").get<std::string>(),
          "JPEG inspection did not distinguish a valid frame header from a truncated file");

    const auto searches = f.web->searches.load();
    const auto fetches = f.web->fetches.load();
    f.batch({search, fetch}, 2);
    check(f.web->searches == searches + 1 && f.web->fetches == fetches, "Parallel calls overspent the remaining budget");
    const auto id = std::to_string(f.lastMessage);
    const auto pending = Json::parse(f.sql("SELECT effect FROM conversation_steps WHERE message_id=" + id + " AND step=1"));
    check(pending[0].at("ok") == true && pending[1].is_null(), "Exhausted batch did not retain its completed call");
    check(f.sql("SELECT CAST(attempts AS TEXT) FROM model_budgets WHERE id='message/" + id + "'") == "2" &&
              f.sql("SELECT CAST(count(*) AS TEXT) FROM model_attempts WHERE purpose='bot_tool' AND message_id=" + id) == "1",
          "Action budget reservation was not durable");
    f.restart();
    f.block(Action::web_search, "public");
    f.approve();
    check(f.web->searches == searches + 1 && f.web->fetches == fetches + 1, "Approval did not execute exactly the pending call");
    check(Conversations(*f.store).pausedConversations("alpha").empty() && f.model.seen[0].at("result").at("accessRevoked") == true &&
              f.model.seen[1].at("result").at("ok") == true,
          "The resumed model did not see the pending result and access revocation");
}

void webEvidenceCannotAuthorizeAnAction(Fixture& f) {
    const auto fetch = call("web_fetch", {{"url", "https://example.com/evidence"}});
    const auto write = call("workspace_write_file", {{"path", "unapproved.txt"}, {"text", "blocked"}});
    const auto read = call("workspace_read_file", {{"path", "allowed.txt"}});
    const auto start = call("start_task", {{"request", "Run a task"}, {"doneWhen", "Done"}});
    const auto path = f.home / "workspaces" / "alpha" / "unapproved.txt";
    const auto taskCount = f.store->tasks("alpha").size();
    f.model.followStep = [&](const Json& steps) {
        if (steps.size() == 1) return std::vector{write, read};
        if (steps.size() == 3) return std::vector{start};
        return std::vector<Decision>{};
    };
    const auto steps = f.batch({fetch});
    f.model.followStep = {};
    check(steps.size() == 4 && steps[1].at("result").at("ok") == false && steps[2].at("result").at("ok") == true &&
              steps[3].at("result").at("ok") == false && !fs::exists(path) && f.store->tasks("alpha").size() == taskCount,
          "A web evidence turn reached a privileged action or lost an independent read");
    check(f.run(write).at("ok") == true && fs::is_regular_file(path), "A new user message could not request an action");
}

void escapedToolHistoryFitsBridgeAndCanBeReread() {
    Fixture f;
    const auto root = f.home / "workspaces" / "alpha";
    fs::create_directories(root);
    const auto data = [](unsigned i) { return "Evidence for slot " + std::to_string(i) + " 根" + std::string(8169, '\x01'); };
    std::vector<Decision> batch;
    for (unsigned i = 0; i < 8; ++i) {
        const auto file = std::to_string(i) + ".txt";
        write(root / file, data(i));
        batch.push_back(call("workspace_read_file", {{"path", file}}));
    }
    constexpr unsigned batches = 19, calls = batches * 8;
    unsigned requests = 0;
    f.model.inspectRequest = [&](const Json& request) {
        ++requests;
        check(request.dump().size() <= 8 * 1024 * 1024, "Serialized request exceeded the production bridge limit");
        const auto context = Json::parse(request.at("input").at(0).at("content").at(0).at("text").get<std::string>());
        check(context.at("event").at("text") == "Check the fixture", "Tool evidence displaced the current user message");
    };
    Json firstRef, secondRef;
    bool recovered = false, revoked = false;
    f.model.followStep = [&](const Json& steps) -> std::vector<Decision> {
        if (steps.size() < calls) return batch;
        if (steps.size() == calls) {
            check(steps.front().at("result").at("evidenceOmitted") == true && steps.front().at("result").at("untrustedData") == true,
                  "The earlier result disappeared without an explicit evidence status");
            firstRef = steps.at(0).at("result").at("resultRef");
            secondRef = steps.at(1).at("result").at("resultRef");
            check(firstRef != secondRef, "Parallel results shared a reference");
            for (unsigned i = 0; i < 8; ++i)
                check(steps.at(calls - 8 + i).at("result").at("text") == data(i), "The latest batch lost evidence or changed call order");
            return {call("read_tool_result", firstRef)};
        }
        const auto& reread = steps.back().at("result").at("result");
        if (steps.size() == calls + 1) {
            check(reread.at("ok") == true && reread.at("text") == data(0) && steps.back().at("result").at("args").at("path") == "0.txt",
                  "The omitted result was lost, rerun, or resolved to the wrong slot after restart");
            recovered = true;
            f.runtime->requestStop();
            return {call("read_tool_result", secondRef)};
        }
        check(reread.at("accessRevoked") == true && !reread.contains("text") && !steps.at(calls).at("result").at("result").contains("text"),
              "A reread or its replay bypassed current workspace authorization");
        revoked = true;
        return {};
    };
    f.batch(batch, batches * 9);
    check(requests == batches, "The stress fixture did not finish all batches before pausing");
    write(root / "0.txt", "Changed on disk after the original read");
    f.restart();
    f.approve();
    check(recovered && !revoked && requests == batches + 2, "The approved turn did not recover the first result before stopping");
    f.restart();
    f.block(Action::workspace_read, "alpha");
    f.runtime->runConversations();
    check(revoked && requests == batches + 3 && Conversations(*f.store).pausedConversations("alpha").empty(),
          "The approved turn did not finish its recovery checks after restart");
    f.model.followStep = {};
    check(f.run(call("read_tool_result", firstRef)).at("ok") == false, "A reference was accepted from another turn");
}

void protectedArgumentsDoNotChangeToolTargets() {
    Fixture f;
    const std::string marker = "protected-model-argument-fixture";
    auto policy = f.store->policy();
    policy.submitSecret(owner, "alpha", policy.requestSecret("alpha", {"argument-fixture"}), marker);
    const auto calls = f.mcp->calls.load();
    const auto workspace = f.home / "workspaces" / "alpha";
    const auto writeFile = [](std::string path) {
        return call("workspace_write_file", {{"path", std::move(path)}, {"text", "Fixture data"}});
    };
    const auto invoke = [](Json arguments) {
        return call("mcp_call_tool", {{"server", "fixture"}, {"name", "lookup"}, {"arguments", std::move(arguments)}});
    };
    const auto steps = f.batch({writeFile(marker + ".txt"), invoke({{"query", {{marker, "protected key"}, {"[redacted]", "public key"}}}}),
                                writeFile("public-arguments.txt")});
    check(steps.size() == 3 && steps[0].at("result").at("ok") == false && steps[1].at("result").at("ok") == false &&
              steps[2].at("result").at("ok") == true && f.mcp->calls == calls && !fs::exists(workspace / (marker + ".txt")) &&
              !fs::exists(workspace / "[redacted].txt") && read(workspace / "public-arguments.txt") == "Fixture data",
          "A protected path or structured key changed an effect, or a safe sibling did not execute");
    const auto persisted =
        f.sql("SELECT decision || result FROM conversation_steps WHERE step=1 AND message_id=" + std::to_string(f.lastMessage));
    check(persisted.find(marker) == persisted.npos && steps.dump().find(marker) == std::string::npos &&
              f.model.content.dump().find(marker) == std::string::npos &&
              Conversations(*f.store).conversation("alpha").dump().find(marker) == std::string::npos,
          "Rejected arguments exposed protected material");

    const Json publicArgs{{"query", {{"[redacted]", "literal key"}}}};
    check(f.run(invoke(publicArgs)).at("data").at("arguments") == publicArgs && f.mcp->calls == calls + 1,
          "A public structured argument changed during dispatch");
}

void freshConversationStepArrays(Fixture& f) {
    const auto message = std::to_string(f.lastMessage);
    check(f.sql("SELECT json_type(decision) || ':' || json_type(result) FROM conversation_steps WHERE message_id=" + message +
                " AND step=1") == "array:array",
          "The fresh database did not persist decisions and results as arrays");
    sqlite3* connection = nullptr;
    const auto opened = sqlite3_open_v2((f.home / "state.db").c_str(), &connection, SQLITE_OPEN_READWRITE, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(connection, sqlite3_close);
    check(opened == SQLITE_OK, "Could not open the fresh database");
    for (const auto* column : {"decision", "result"}) {
        const auto sql = "UPDATE conversation_steps SET " + std::string(column) + "='{}' WHERE message_id=" + message;
        check(sqlite3_exec(db.get(), sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_CONSTRAINT,
              "The fresh database accepted an object in an array column");
    }
}

void mcpSessionPagination(McpTools& adapter) {
    const auto first = adapter.discover("alpha", "one", McpCollection::tools, {});
    const auto cursor = first.at("nextCursor").get<std::string>();
    check(first.at("tools").at(0).at("name") == "extension" && cursor != "session-bound",
          "The first page did not return an opaque continuation");
    const auto expectDenied = [&](std::string_view bot, std::string_view server, McpCollection collection) {
        try {
            adapter.discover(bot, server, collection, cursor);
        } catch (const Conflict&) {
            return;
        }
        throw std::runtime_error("A discovery cursor was accepted outside its original scope or after use");
    };
    expectDenied("beta", "one", McpCollection::tools);
    expectDenied("alpha", "two", McpCollection::tools);
    expectDenied("alpha", "one", McpCollection::resources);
    const auto next = adapter.discover("alpha", "one", McpCollection::tools, cursor);
    check(next.at("tools").size() == 2 && !next.contains("nextCursor"), "MCP continuation did not reach the original session");
    expectDenied("alpha", "one", McpCollection::tools);
    check(adapter.readOnlyHint("one", "extension") && !adapter.readOnlyHint("two", "extension") &&
              !adapter.readOnlyHint("one", "tool-failure") && !adapter.readOnlyHint("one", "untyped-hint") &&
              !adapter.readOnlyHint("one", "compatible-content"),
          "MCP tool hints were lost, misread, or shared across servers");
}

void mcpToolOutcomes(Fixture& f) {
    const auto invoke = [](std::string_view name, std::string_view server = "one") {
        return call("mcp_call_tool", {{"server", server}, {"name", name}, {"arguments", Json::object()}});
    };
    const auto expectUnknown = [](const Json& result, std::string_view name) {
        check(result.at("ok") == false && result.value("delivery", "") == "unknown" && !result.contains("data"),
              "Malformed MCP result was exposed as evidence: " + std::string(name));
    };

    f.batch({invoke("numeric-content")}, 2);
    f.restart();
    f.approve();
    expectUnknown(f.model.seen.at(0).at("result"), "numeric-content after restart");

    for (const auto* name : {"missing-content", "invalid-block", "invalid-resource", "invalid-resource-mime-type", "invalid-annotations",
                             "invalid-structured-content", "invalid-is-error"})
        expectUnknown(f.run(invoke(name)), name);

    f.store->policy().setMode(owner, "supervised");
    const auto success = f.run(invoke("extension"));
    check(success.at("ok") == true && success.at("untrustedData") == true &&
              success.at("data").at("content").at(0).at("text") == "Tool evidence" &&
              success.at("data").at("content").at(1).at("payload") == 7 && success.at("data").at("structuredContent").at("count") == 1,
          "Valid MCP content and extensions did not reach the model");

    const auto approveMcp = [&](std::string_view name) {
        check(f.batch({invoke(name)}).is_null(), "An MCP tool that required approval ran before approval");
        const auto items = Inbox(*f.store).inbox(owner);
        check(items.size() == 1 && items.at(0).at("itemKind") == "policy_approval", "MCP tool did not create an approval");
        const auto& item = items.at(0);
        Inbox(*f.store).decideInbox(owner, {"alpha", InboxKind::policy_approval, item.at("itemId"), item.at("waitId"), 0, 0, 0},
                                    "allow_once");
        f.runtime->runConversations();
        return f.model.seen.at(0).at("result");
    };
    f.store->policy().changeRule(owner, RuleKind::ask, Action::mcp_invoke, "one", true);
    check(approveMcp("extension").at("ok") == true, "Ask first did not take precedence over a read-only hint");
    f.store->policy().changeNeverRule(owner, Action::mcp_invoke, "one", true);
    check(f.run(invoke("extension")).at("ok") == false && Inbox(*f.store).inbox(owner).empty(),
          "Never did not take precedence over a read-only hint and Ask first");
    f.store->policy().changeNeverRule(owner, Action::mcp_invoke, "one", false);
    f.store->policy().changeRule(owner, RuleKind::ask, Action::mcp_invoke, "one", false);

    const auto compatible = approveMcp("compatible-content");
    check(compatible.at("ok") == true && compatible.at("data").at("content").size() == 3,
          "Valid resource alternatives or legacy extensions were rejected");

    const auto failure = approveMcp("tool-failure");
    check(failure.at("ok") == false && !failure.contains("delivery") && failure.at("untrustedData") == true &&
              failure.at("data").at("content").at(0).at("text") == "Tool failed" && !failure.at("data").contains("isError"),
          "Definite MCP tool failure did not survive credential redaction");

    f.store->policy().setMode(owner, "full");
    const auto rejected = f.run(invoke("rejected"));
    check(rejected.at("ok") == false && !rejected.contains("delivery") && rejected.contains("error"),
          "JSON-RPC rejection was classified as an ambiguous dispatch");

    const auto initFailure = f.run(invoke("never-dispatched", "failed-init"));
    check(initFailure.at("ok") == false && !initFailure.contains("delivery") && initFailure.contains("error"),
          "MCP initialization failure was classified as an ambiguous dispatch");
}

void mcpLoopback(Fixture& f, std::string_view url) {
    write(f.home / "bot-tools.json", Json{{"mcpServers",
                                           {{"one", {{"url", url}, {"tokenEnv", "FLOCK_MCP_TEST_TOKEN"}}},
                                            {"two", {{"url", url}}},
                                            {"failed-init", {{"url", std::string(url) + "/failed-init"}}}}}}
                                         .dump());
    f.tools->mcp = app::loadBotTools(f.home)->mcp;
    mcpSessionPagination(*f.tools->mcp);
    mcpToolOutcomes(f);
}

void approvalWhileAnotherTurnIsInferring(Fixture& f) {
    f.block(Action::web_search, "public", false);
    f.block(Action::web_search, "public", false);
    const auto search = [](std::string query) { return call("web_search", {{"query", query}}); };
    f.batch({search("older")}, 1);
    const auto paused = Conversations(*f.store).pausedConversations("alpha").at(0);
    f.model.beforeTurn = [&](const ModelInput& input) {
        if (input.event.at("messageId") == f.lastMessage && input.event.at("turn").empty()) f.approvePaused(paused);
    };
    f.batch({search("newer")});
    f.model.beforeTurn = {};
    const auto& queries = f.web->searchQueries;
    check(queries.size() >= 2 && queries[queries.size() - 2] == "older" && queries.back() == "newer",
          "A newer tool action overtook a resumed turn");
    f.approve();
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--tool-history") {
            escapedToolHistoryFitsBridgeAndCanBeReread();
            return 0;
        }
        Fixture fixture;
        if (argc == 2) {
            mcpLoopback(fixture, argv[1]);
            return 0;
        }
        protectedArgumentsDoNotChangeToolTargets();
        workspaceFiles(fixture);
        mountedWorkspaceDirectoryIdentity();
        externalTools(fixture);
        approvalWhileAnotherTurnIsInferring(fixture);
        webEvidenceCannotAuthorizeAnAction(fixture);
        freshConversationStepArrays(fixture);
        std::cout << "Bot workspace and tool acceptance passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
