#include "flock/access.hpp"
#include "flock/board_inbox.hpp"
#include "flock/workspace_tools.hpp"
#include "bot_tools.hpp"
#include "model.hpp"
#include "../src/workspace_internal.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sqlite3.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace flockv0;
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

const Person owner{"owner",true};
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
    check(file.good(),"Could not write fixture data");
}
std::string read(const fs::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file),{}};
}

Decision call(std::string tool, Json args = Json::object()) {
    return {std::move(tool),std::move(args),"Collect the requested evidence"};
}

struct Provider : Model {
    std::vector<Decision> next;
    Json seen;
    Json content;
    std::function<void(const ModelInput&)> beforeTurn;
    std::function<std::vector<Decision>(const Json&)> followStep;
    ModelTurn decide(const ModelInput& input) override {
        auto request = app::responseRequest(input);
        auto output = Json::array();
        auto emit = [&](const Decision& action) {
            auto args = action.args;
            args["reason"] = action.reason;
            output.push_back({{"type","function_call"},{"namespace","flock"},{"status","completed"},
                              {"name",action.tool},{"arguments",args.dump()}});
        };
        if (input.event.at("kind") != "conversation_turn") {
            emit(call("delegate",{{"instruction","Read private history"},{"doneWhen","Private fixture checked"}}));
        } else {
            if (beforeTurn) beforeTurn(input);
            check(request.at("parallel_tool_calls") == true,"Conversation schema did not enable parallel calls");
            check(request.at("instructions").get<std::string>().find("untrustedData") != std::string::npos,
                  "Tool output trust was not explained in the prompt");
            const auto& steps = input.event.at("turn");
            if (steps.empty()) {
                for (const auto& action : next) emit(action);
            } else {
                seen = steps;
                content = request.at("input").at(0).at("content");
                auto follow = followStep ? followStep(steps) : std::vector<Decision>{};
                if (follow.empty()) emit(call("reply",{{"text","Checked."}}));
                else for (const auto& action : follow) emit(action);
            }
        }
        return app::responseDecision(output,request);
    }
};

struct LocalT3 : T3 {
    bool launched = false;
    std::string authGeneration(const std::string&,const Json&) override { return "connected"; }
    Delivery send(const Command&) override {
        launched = true;
        return {Delivery::Kind::Accepted,{"private-thread","private-run",{}},{}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&,const Json&,const RunRef&) override { return {}; }
    void interrupt(const std::string&,const Json&,const RunRef&) override {}
    std::optional<SessionMetadata> sessionMetadata(const SessionTarget&,bool) override {
        return SessionMetadata{std::string(historyMarker),"private-project","private-project","running",
                               "2026-11-21T00:00:00Z","2026-11-21T00:00:00Z",{},false};
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
        if (!gate.wait_for(lock,2s,[&] { return arrived == 2; }))
            throw Conflict("Independent web calls did not overlap");
    }
    Json search(std::string_view query) override {
        ++searches;
        {
            std::lock_guard lock(mutex);
            searchQueries.emplace_back(query);
        }
        rendezvous();
        if (query == "complete links")
            return {{"results",Json::array({{{"title",std::string(250,'x') + std::string(secretMarker)},
                              {"url","https://example.com/vault-"},{"description","Evidence"}}})}};
        return {{"query",query},{"text","Untrusted search evidence"}};
    }
    Json fetch(std::string_view url) override {
        ++fetches;
        rendezvous();
        return {{"url",url},{"text",url.ends_with("/vault-") ? "Partial value: vault-" : "Untrusted page evidence"},
                {"truncated",url.ends_with("/vault-")}};
    }
};

struct FakeMcp : McpTools {
    std::atomic<int> discoveries{0}, calls{0}, reads{0};
    bool hasServer(std::string_view server) const override { return server == "fixture" || server == "private"; }
    Json discover(std::string_view, std::string_view, McpCollection collection, std::string_view cursor) override {
        ++discoveries;
        if (collection == McpCollection::resources) return {{"resources",{"fixture://data"}}};
        if (cursor == "tools-page-2") return {{"tools",{"second"}}};
        return {{"tools",{"lookup"}},{"nextCursor","tools-page-2"}};
    }
    Json invoke(std::string_view, std::string_view name, const Json& args) override {
        ++calls;
        return {{"toolName",name},{"arguments",args}};
    }
    Json readResource(std::string_view, std::string_view uri) override { ++reads; return {{"uri",uri}}; }
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
        check(name != nullptr,"Could not create test workspace");
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
        store = std::make_unique<Store>(home/"state.db");
        RuntimeOptions options;
        options.botTools = tools;
        runtime = std::make_unique<Runtime>(*store,model,t3,[](auto) { return std::string{}; },options);
    }
    Intake intake(std::string bot = "alpha", Id limit = 20) const {
        return {std::move(bot),"","test-environment","private-project",
                {{"modelCallLimit",limit},{"model","test"},{"modelProvider","chatgpt"}}};
    }
    void grant(Scope scope, std::string_view bot = "alpha") {
        store->access().resourceGrant(owner,bot,scope,GrantChange::grant);
    }
    Json batch(std::vector<Decision> calls, Id budget = 20, std::string bot = "alpha") {
        model.next = std::move(calls);
        model.seen = nullptr;
        lastMessage = store->receive(owner,intake(std::move(bot),budget),std::to_string(++seq),"Check the fixture").at("messageId");
        runtime->runConversations();
        return model.seen;
    }
    Json run(Decision action, std::string bot = "alpha") {
        const auto steps = batch({std::move(action)},20,std::move(bot));
        check(steps.is_array() && steps.size() == 1,"Expected one tool result");
        return steps[0].at("result");
    }
    void approvePaused(const Json& pause) {
        const Id wait = pause.at("messageId");
        store->decideInbox(owner,{"alpha",InboxKind::conversation_model_budget,pause.at("replyToMessageId"),
                                  wait,0,0,wait},"approve");
    }
    void approve() {
        const auto paused = store->pausedConversations("alpha");
        check(paused.size() == 1,"Expected one paused turn");
        approvePaused(paused.at(0));
        runtime->runConversations();
    }
    std::string sql(const std::string& query) const {
        sqlite3* connection = nullptr;
        const auto opened = sqlite3_open_v2((home/"state.db").c_str(),&connection,SQLITE_OPEN_READONLY,nullptr);
        const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(connection,sqlite3_close);
        check(opened == SQLITE_OK,"Fixture database open failed");
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(db.get(),query.c_str(),-1,&statement,nullptr) == SQLITE_OK,"Fixture SQL preparation failed");
        const std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> row(statement,sqlite3_finalize);
        check(sqlite3_step(row.get()) == SQLITE_ROW,"Fixture SQL row missing");
        return reinterpret_cast<const char*>(sqlite3_column_text(row.get(),0));
    }
    Json status(Id id, std::string bot = "alpha") {
        return run(call("workspace_command_status",{{"commandId",id}}),std::move(bot));
    }
    Id command(Json argv) {
        const auto result = run(call("workspace_run_command",{{"argv",std::move(argv)}}));
        check(result.value("ok",false),result.dump());
        check(result.at("state") == "running","Command start did not return running");
        return result.at("commandId");
    }
    Json until(Id id, const std::function<bool(const Json&)>& predicate) {
        for (auto deadline = std::chrono::steady_clock::now()+3s; std::chrono::steady_clock::now() < deadline;) {
            auto result = status(id);
            if (predicate(result)) return result;
            std::this_thread::sleep_for(15ms);
        }
        throw std::runtime_error("Workspace command did not reach the expected state: " + status(id).dump());
    }
};

void workspaceAndLifecycle(Fixture& f) {
    const auto normal = call("workspace_write_file",{{"path","allowed.txt"},{"text","workspace-only-marker"}});
    check(!f.run(normal).at("ok").get<bool>(),"Workspace write occurred without a grant");
    f.grant({"workspace","alpha","write"});
    f.grant({"workspace","alpha","read"});
    check(f.run(normal).at("ok") == true,"Granted workspace write failed");
    const auto root = f.home/"workspaces"/"alpha";
    const auto marker = f.run(call("workspace_search",{{"query","workspace-only-marker"}}));
    check(marker.at("files") == Json::array({"allowed.txt"}),"Workspace search missed its marker");
    check(f.run(call("workspace_read_file",{{"path","allowed.txt"}})).at("text") == "workspace-only-marker",
          "Workspace read missed its content");
    write(root/"script","#!/bin/sh\n");
    check(chmod((root/"script").c_str(),0700) == 0,"Could not make fixture script executable");
    check(f.run(call("workspace_write_file",{{"path","script"},{"text","#!/bin/sh\nprintf updated"}})).at("ok") == true &&
          access((root/"script").c_str(),X_OK) == 0,"Workspace edit lost executable permissions");
    check(!f.run(call("workspace_run_command",{{"argv",Json::array({"pwd"})}})).at("ok").get<bool>(),
          "Workspace command ran without an exec grant");
    f.grant({"workspace","alpha","exec"});
    const auto finished = [](const Json& state) { return state.at("state") == "exited"; };
    auto allowed = f.until(f.command(Json::array({"cat","allowed.txt"})),finished);
    check(allowed.at("stdout") == "workspace-only-marker" && allowed.at("exitCode") == 0,allowed.dump());
#ifdef __APPLE__
    const auto pipeline = f.until(f.command(Json::array({"sh","-c","/bin/echo x | /usr/bin/tee spawned.txt"})),finished);
    check(pipeline.at("exitCode") != 0 && !fs::exists(root/"spawned.txt"),
          "The macOS single-process sandbox allowed a pipeline to spawn children");
#elif defined(__linux__)
    const auto absolute = f.until(f.command(Json::array({"/bin/cat","allowed.txt"})),finished);
    check(absolute.at("stdout") == "workspace-only-marker" && absolute.at("exitCode") == 0,absolute.dump());
#endif

    const auto secret = f.store->access().requestSecret("beta",{"fixture-key"});
    f.store->access().submitSecret(owner,"beta",secret,secretMarker);
    const auto secretFile = fs::directory_iterator(f.home/"secrets")->path();
    fs::create_directories(f.home/"workspaces"/"beta");
    const auto peer = f.home/"workspaces"/"beta"/"private";
    write(peer,peerMarker);
    const auto history = f.home/"private-session.jsonl";
    write(history,historyMarker);
    auto privateIntake = f.intake("beta");
    privateIntake.request = dbMarker;
    f.grant({"environment","test-environment","launch"},"beta");
    f.store->submit(privateIntake);
    for (int i = 0; i < 12 && !f.t3.launched; ++i) f.runtime->tick();
    check(f.t3.launched,"Private T3 fixture did not start");
    const auto sessionScope = Scope::sessionRead("test-environment","private-thread");
    f.grant(sessionScope);
    const auto searchSession = call("flock_session_search");
    check(f.run(searchSession).at("sessions").size() == 1,"Session fixture did not appear with its grant");
    f.store->access().resourceGrant(owner,"alpha",sessionScope,GrantChange::revoke);
    check(f.run(searchSession).at("sessions").empty(),"Revoked Session was still readable");
    f.sql("SELECT 1 FROM tasks WHERE data LIKE '%state-only-marker%'");
    check(read(secretFile) == secretMarker,"Secret marker was not persisted in the vault");

    const std::array<fs::path,4> paths{f.home/"state.db",secretFile,peer,history};
    const std::array<std::string_view,4> markers{dbMarker,secretMarker,peerMarker,historyMarker};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto& target = paths[i];
        check(f.run(call("workspace_read_file",{{"path",target.string()}})).at("ok") == false,
              "Absolute private file was readable");
        check(f.run(call("workspace_read_file",{{"path","../../"+target.filename().string()}})).at("ok") == false,
              "Traversal was accepted");
        const auto raw = f.until(f.command(Json::array({"cat",target.string()})),finished);
        check(raw.at("exitCode") != 0 && raw.at("stdout") == "",raw.dump());
        const auto alias = root/"escape";
        fs::create_symlink(target,alias);
        check(f.run(call("workspace_read_file",{{"path","escape"}})).at("ok") == false,"Symlink read escaped");
        check(f.run(call("workspace_write_file",{{"path","escape"},{"text","overwritten"}})).at("ok") == false,
              "Symlink write escaped");
        check(f.run(call("workspace_search",{{"query",markers[i]}})).at("files").empty(),"Symlink search escaped");
        const auto linked = f.run(call("workspace_run_command",{{"argv",Json::array({"cat","escape"})}}));
        check(linked.at("ok") == false,"Command accepted a symlink to private files");
        fs::remove(alias);
    }

    const auto hardlink = f.until(f.command(Json::array({"ln",peer.string(),"private-link"})),finished);
    check(hardlink.at("exitCode") != 0 && !fs::exists(root/"private-link"),"Command linked a private file into its workspace");

    f.store->access().resourceGrant(owner,"alpha",{"workspace","alpha","write"},GrantChange::revoke);
    const auto readonly = f.until(f.command(Json::array({"touch","ungranted-write"})),finished);
    check(readonly.at("exitCode") != 0 && !fs::exists(root/"ungranted-write"),"Workspace exec bypassed its write grant");
    f.grant({"workspace","alpha","write"});

    setenv("FLOCK_SECRET_WORKSPACE_TEST","must-not-be-inherited",1);
    const auto env = f.until(f.command(Json::array({"printenv"})),finished);
    unsetenv("FLOCK_SECRET_WORKSPACE_TEST");
    check(env.at("exitCode") == 0 && env.dump().find("must-not-be-inherited") == std::string::npos,
          "Workspace command inherited Flock's environment");

    const auto capturedSecret = f.store->access().requestSecret("alpha",{"capture-proof"});
    f.store->access().submitSecret(owner,"alpha",capturedSecret,"command-redaction-fixture");
    write(root/"captured","command-redaction-fixture");
    const auto secretCommand = f.command(Json::array({"cat","captured"}));
    check(f.until(secretCommand,finished).at("stdout") == "[redacted]" &&
          f.sql("SELECT stdout FROM workspace_commands WHERE id=" + std::to_string(secretCommand)) == "[redacted]",
          "Command captured a protected value in persistent state");

    const auto streaming = f.command(Json::array({"sh","-c",
        "i=0; while [ $i -lt 300 ]; do printf '%064d' 1; printf x >&2; i=$((i+1)); done; exec /bin/sleep 60"}));
    const auto large = f.until(streaming,[](const Json& status) { return status.at("truncated") == true; });
    check(large.at("state") == "running","Command was not running while output was captured");
    check(large.at("stdout").get<std::string>().size()+large.at("stderr").get<std::string>().size() <= workspaceOutputLimit,
          "Command output exceeded its byte ceiling");
    f.grant({"workspace","beta","exec"},"beta");
    check(f.status(streaming,"beta").at("ok") == false,"Another Bot could read an active command");
    const auto stop = f.run(call("workspace_stop_command",{{"commandId",streaming}}));
    check(stop.at("state") == "stop_requested","Stop did not record stop_requested");
    f.until(streaming,finished);

    const auto mutating = f.command(Json::array({"sh","-c","printf x >> mutation; exec /bin/sleep 60"}));
    f.until(mutating,[&](const Json&) { return read(root/"mutation") == "x"; });
    f.restart();
    check(f.status(mutating).at("state") == "unknown_after_restart","Running command was not reconciled on restart");
    f.runtime->runConversations();
    check(read(root/"mutation") == "x","An unknown-state mutating command ran again");
}

void mountedWorkspaceDirectoryIdentity() {
#if defined(__linux__) || defined(__APPLE__)
#ifdef __linux__
    const auto* mount = "proc";
#else
    const auto* mount = "dev";
#endif
    const auto root = open("/",O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(root >= 0,"Could not open the root directory");
    int child;
    try { child = openExactChildDirectory(root,mount); }
    catch (...) { close(root); throw; }
    struct stat rootInfo{}, childInfo{};
    const bool distinct = fstat(root,&rootInfo) == 0 && fstat(child,&childInfo) == 0 &&
                          rootInfo.st_dev != childInfo.st_dev;
    close(child);
    close(root);
    check(distinct,"The test directory is not a separate mount");
#endif
}

void externalTools(Fixture& f) {
    const auto search = call("web_search",{{"query","public fixture"}});
    const auto fetch = call("web_fetch",{{"url","https://example.com/evidence"}});
    check(f.run(search).at("ok") == false && f.web->searches == 0,"Ungranted web search reached its adapter");
    f.grant({"web","public","search"});
    f.grant({"web","example.com","fetch"});
    f.web->synchronize = true;
    const auto paired = f.batch({search,fetch});
    f.web->synchronize = false;
    check(paired[0].at("tool") == "web_search" && paired[1].at("tool") == "web_fetch",
          "Parallel calls returned out of order");
    for (const auto& step : paired)
        check(step.at("result").at("ok") == true && step.at("result").at("untrustedData") == true,
              "Concurrent web result was invalid or untagged");
    check(f.run(call("web_fetch",{{"url","https://other.example/evidence"}})).at("ok") == false && f.web->fetches == 1,
          "Web fetch bypassed its host grant");
    check(f.run(call("web_fetch",{{"url","https://example.com@127.0.0.1/"}})).at("ok") == false,
          "An invalid or internal fetch target was accepted");
    const auto redacted = f.run(call("web_fetch",{{"url","https://example.com/vault-"}})).at("data");
    check(redacted.at("url") == "https://example.com/vault-" &&
          redacted.at("text") == "Partial value: [redacted]",
          "A complete URL and a truncated body did not receive different redaction");
    const auto link = f.run(call("web_search",{{"query","complete links"}})).at("data").at("results").at(0);
    check(link.at("url") == "https://example.com/vault-" && link.at("title") == std::string(250,'x') + "[redac",
          "Search did not preserve complete URLs and redact titles before bounding them");

    for (const auto* op : {"discover","invoke","read_resource"}) f.grant({"mcp_server","fixture",op});
    const auto discovery = call("mcp_discover",{{"server","fixture"},{"collection","tools"}});
    const auto discover = f.run(discovery);
    const auto invoke = call("mcp_call_tool",{{"server","fixture"},{"name","lookup"},{"arguments",{{"query","x"}}}});
    const auto resource = call("mcp_read_resource",{{"server","fixture"},{"uri","fixture://data"}});
    check(discover.at("data").at("tools") == Json::array({"lookup"}) && discover.at("untrustedData") == true,
          "MCP discovery result was invalid");
    auto nextPage = discovery;
    nextPage.args["cursor"] = discover.at("data").at("nextCursor");
    check(f.run(nextPage).at("data").at("tools") == Json::array({"second"}),"MCP tool cursor did not reach the adapter");
    const auto invoked = f.run(invoke);
    check(invoked.at("data").at("toolName") == "lookup" && invoked.at("untrustedData") == true,"MCP invocation failed");
    const auto readResource = f.run(resource);
    check(readResource.at("data").at("uri") == "fixture://data" && readResource.at("untrustedData") == true,
          "MCP resource read failed");
    for (const auto* op : {"discover","invoke","read_resource"})
        f.store->access().resourceGrant(owner,"alpha",{"mcp_server","fixture",op},GrantChange::revoke);
    check(f.run(discovery).at("ok") == false && f.mcp->discoveries == 2,
          "Revoked MCP discovery reached the adapter");
    check(f.run(invoke).at("ok") == false && f.mcp->calls == 1,"Revoked MCP invocation reached the adapter");
    check(f.run(resource).at("ok") == false && f.mcp->reads == 1,"Revoked MCP resource read reached the adapter");

    const auto skill = f.home/"fixture-skill";
    fs::create_directory(skill);
    write(skill/"SKILL.md","Read this fixture skill.");
    fs::create_symlink(f.home/"state.db",skill/"escape");
    f.tools->skills.emplace("fixture",skill);
    f.grant({"skill","catalog","list"});
    check(f.run(call("skill_list")).at("data").empty(),"Skill catalog leaked a denied skill");
    f.grant({"skill","fixture","read"});
    check(f.run(call("skill_list")).at("data") == Json::array({{{"name","fixture"}}}),"Skill catalog omitted an allowed skill");
    const auto instructions = f.run(call("skill_read",{{"skill","fixture"}}));
    check(instructions.at("data").at("text") == "Read this fixture skill." && instructions.at("untrustedData") == true,
          "Skill read failed");
    check(f.run(call("skill_read",{{"skill","fixture"},{"path","escape"}})).at("ok") == false,"Skill read traversed a symlink");
    check(f.run(call("skill_read",{{"skill","fixture"},{"path","../state.db"}})).at("ok") == false,"Skill read traversed its root");

    const auto media = Json::array({
        {{"name","pixel.png"},{"mimeType","image/png"},{"dataBase64","iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"}},
        {{"name","tone.wav"},{"mimeType","audio/wav"},{"dataBase64","UklGRiYAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQIAAACAgA=="}},
        {{"name","corrupt.png"},{"mimeType","image/png"},{"dataBase64","iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1Pe"}},
        {{"name",secretMarker},{"mimeType","audio/mpeg"},{"dataBase64","SUQzdmF1bHQtb25seS1tYXJrZXI="}}
    });
    f.model.next = {call("reply",{{"text","Message received."}})};
    const auto message = f.store->receive(owner,f.intake(),"media","Please inspect these photos and audio",0,media).at("messageId");
    f.runtime->runConversations();
    const auto image = call("inspect_attachment",{{"messageId",message},{"attachmentId","1"}});
    const auto audio = call("inspect_attachment",{{"messageId",message},{"attachmentId","2"}});
    check(f.run(image).at("ok") == false,"Attachment was inspected without its grant");
    check(f.model.content.size() == 1,"Ungranted image reached the model request");
    f.grant({"attachment","alpha","inspect"});
    f.grant({"attachment","beta","inspect"},"beta");
    const auto metadata = f.batch({image,audio});
    check(metadata[0].at("result").at("data").at("kind") == "image" &&
          metadata[0].at("result").at("data").at("width") == 1 &&
          metadata[0].at("result").at("untrustedData") == true &&
          metadata[1].at("result").at("data").at("kind") == "audio" &&
          metadata[1].at("result").at("data").at("sampleRateHz") == 8000 &&
          metadata[1].at("result").at("untrustedData") == true,"Media inspection did not return scoped metadata");
    check(f.model.content.size() == 3 && f.model.content[2].at("type") == "input_image" &&
          f.model.content[2].at("image_url") == "data:image/png;base64," + media[0].at("dataBase64").get<std::string>(),
          "An authorized attachment did not reach the production image input schema");
    check(f.run(image,"beta").at("ok") == false,"Another Bot could inspect the attachment");
    const auto corrupt = f.run(call("inspect_attachment",{{"messageId",message},{"attachmentId","3"}}));
    check(corrupt.at("ok") == false && f.model.content.size() == 1,
          "A corrupt image reached the model or interrupted the turn");
    const auto opaque = f.run(call("inspect_attachment",{{"messageId",message},{"attachmentId","4"}}));
    check(opaque.at("ok") == true && opaque.at("data").at("name") == secretMarker,
          "Attachment upload or inspection revealed a match against another Bot's vault");
    check(f.store->receipt(owner,"alpha","media","Please inspect these photos and audio",0,media).has_value(),
          "Attachment receipt was not idempotent");
    const auto jpeg = Json::array({
        {{"name","pixel.jpg"},{"mimeType","image/jpeg"},
         {"dataBase64","/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIx"
                       "wcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAAAAAAAAAAAAAAAP/EABQQAQAAAAAA"
                       "AAAAAAAAAAAAAAD/2gAIAQEAAD8AP//Z"}},
        {{"name","corrupt.jpg"},{"mimeType","image/jpeg"},{"dataBase64","/9j/4AAQSkZJRgABAQAAAQABAAA="}}
    });
    f.model.next = {call("reply",{{"text","Message received."}})};
    const auto pictures = f.store->receive(owner,f.intake(),"jpeg","Please inspect these JPEGs",0,jpeg).at("messageId");
    f.runtime->runConversations();
    const auto jpegViews = f.batch({call("inspect_attachment",{{"messageId",pictures},{"attachmentId","1"}}),
                                    call("inspect_attachment",{{"messageId",pictures},{"attachmentId","2"}})});
    check(jpegViews[0].at("result").at("data").at("width") == 1 &&
          jpegViews[1].at("result").at("ok") == false && f.model.content.size() == 3 &&
          f.model.content.at(2).at("image_url") == "data:image/jpeg;base64," + jpeg[0].at("dataBase64").get<std::string>(),
          "JPEG decoding did not distinguish valid pixels from a truncated file");

    const auto searches = f.web->searches.load();
    const auto fetches = f.web->fetches.load();
    f.batch({search,fetch},2);
    check(f.web->searches == searches+1 && f.web->fetches == fetches,"Parallel calls overspent the remaining budget");
    const auto id = std::to_string(f.lastMessage);
    const auto pending = Json::parse(f.sql("SELECT effect FROM conversation_steps WHERE message_id="+id+" AND step=1"));
    check(pending[0].at("ok") == true && pending[1].is_null(),"Exhausted batch did not retain its completed call");
    check(f.sql("SELECT CAST(attempts AS TEXT) FROM model_budgets WHERE id='message/"+id+"'") == "2" &&
          f.sql("SELECT CAST(count(*) AS TEXT) FROM model_attempts WHERE purpose='bot_tool' AND message_id="+id) == "1",
          "Action budget reservation was not durable");
    f.restart();
    f.store->access().resourceGrant(owner,"alpha",{"web","public","search"},GrantChange::revoke);
    f.approve();
    check(f.web->searches == searches+1 && f.web->fetches == fetches+1,
          "Approval did not execute exactly the pending call");
    f.approve();
    check(f.model.seen[0].at("result").at("accessRevoked") == true &&
          f.model.seen[1].at("result").at("ok") == true,"Revoked result reached the resumed model");
}

void webEvidenceCannotAuthorizeAnAction(Fixture& f) {
    f.grant({"web","example.com","fetch"});
    f.grant({"workspace","alpha","write"});
    f.grant({"workspace","alpha","read"});
    const auto fetch = call("web_fetch",{{"url","https://example.com/evidence"}});
    const auto write = call("workspace_write_file",{{"path","unapproved.txt"},{"text","blocked"}});
    const auto read = call("workspace_read_file",{{"path","allowed.txt"}});
    const auto start = call("start_task",{{"request","Run a task"},{"doneWhen","Done"}});
    const auto path = f.home/"workspaces"/"alpha"/"unapproved.txt";
    const auto taskCount = f.store->tasks("alpha").size();
    f.model.followStep = [&](const Json& steps) {
        if (steps.size() == 1) return std::vector{write,read};
        if (steps.size() == 3) return std::vector{start};
        return std::vector<Decision>{};
    };
    const auto steps = f.batch({fetch});
    f.model.followStep = {};
    check(steps.size() == 4 && steps[1].at("result").at("ok") == false && steps[2].at("result").at("ok") == true &&
          steps[3].at("result").at("ok") == false && !fs::exists(path) && f.store->tasks("alpha").size() == taskCount,
          "A web evidence turn reached a privileged action or lost an independent read");
    check(f.run(write).at("ok") == true && fs::is_regular_file(path),"A new user message could not request an action");
}

void freshConversationStepArrays(Fixture& f) {
    const auto message = std::to_string(f.lastMessage);
    check(f.sql("SELECT json_type(decision) || ':' || json_type(result) FROM conversation_steps WHERE message_id=" +
                message + " AND step=1") == "array:array","The fresh database did not persist decisions and results as arrays");
    sqlite3* connection = nullptr;
    const auto opened = sqlite3_open_v2((f.home/"state.db").c_str(),&connection,SQLITE_OPEN_READWRITE,nullptr);
    const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(connection,sqlite3_close);
    check(opened == SQLITE_OK,"Could not open the fresh database");
    for (const auto* column : {"decision","result"}) {
        const auto sql = "UPDATE conversation_steps SET " + std::string(column) + "='{}' WHERE message_id=" + message;
        check(sqlite3_exec(db.get(),sql.c_str(),nullptr,nullptr,nullptr) == SQLITE_CONSTRAINT,
              "The fresh database accepted an object in an array column");
    }
}

void mcpSessionPagination(Fixture& f, std::string_view url) {
    write(f.home/"bot-tools.json",Json{{"mcpServers",{{"one",{{"url",url}}},{"two",{{"url",url}}}}}}.dump());
    const auto adapter = app::loadBotTools(f.home)->mcp;
    const auto first = adapter->discover("alpha","one",McpCollection::tools,{});
    const auto cursor = first.at("nextCursor").get<std::string>();
    check(first.at("tools") == Json::array({"first"}) && cursor != "session-bound",
          "The first page did not return an opaque continuation");
    const auto expectDenied = [&](std::string_view bot, std::string_view server, McpCollection collection) {
        try { adapter->discover(bot,server,collection,cursor); }
        catch (const Conflict&) { return; }
        throw std::runtime_error("A discovery cursor was accepted outside its original scope or after use");
    };
    expectDenied("beta","one",McpCollection::tools);
    expectDenied("alpha","two",McpCollection::tools);
    expectDenied("alpha","one",McpCollection::resources);
    const auto next = adapter->discover("alpha","one",McpCollection::tools,cursor);
    check(next.at("tools") == Json::array({"second"}) && !next.contains("nextCursor"),
          "MCP continuation did not reach the original session");
    expectDenied("alpha","one",McpCollection::tools);
}

void approvalWhileAnotherTurnIsInferring(Fixture& f) {
    f.grant({"web","public","search"});
    const auto search = [](std::string query) { return call("web_search",{{"query",query}}); };
    f.batch({search("older")},1);
    const auto paused = f.store->pausedConversations("alpha").at(0);
    f.model.beforeTurn = [&](const ModelInput& input) {
        if (input.event.at("messageId") == f.lastMessage && input.event.at("turn").empty())
            f.approvePaused(paused);
    };
    f.batch({search("newer")});
    f.model.beforeTurn = {};
    const auto& queries = f.web->searchQueries;
    check(queries.size() >= 2 && queries[queries.size()-2] == "older" && queries.back() == "newer",
          "A newer tool action overtook a resumed turn");
    f.approve();
}
}

int main(int argc, char** argv) {
    try {
        Fixture fixture;
        if (argc == 2) {
            mcpSessionPagination(fixture,argv[1]);
            return 0;
        }
        workspaceAndLifecycle(fixture);
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
