#include "flock/channels.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

using namespace flockv0;
namespace {
const Person root{"root",true}, owner{"fixture-user",false};
const std::string image = decodeAttachment(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAACklEQVR4nGNgAAAAAgABSK+kcQAAAABJRU5ErkJggg==",128);

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void rejects(const std::function<void()>& action) {
    try { action(); }
    catch (const Conflict&) { return; }
    throw std::runtime_error("Expected rejection");
}

struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("flock-channels-" + std::to_string(unixTime()) +
        "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(path); }
    ~Temp() { std::filesystem::remove_all(path); }
};

struct SessionAdapter final : T3 {
    int launches = 0;
    std::string authGeneration(const std::string&, const Json&) override { return "auth"; }
    Delivery send(const Command&) override {
        ++launches;
        return {Delivery::Kind::Accepted,{"thread-"+std::to_string(launches),"run-"+std::to_string(launches),""},{}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef&) override { return {}; }
    void interrupt(const std::string&, const Json&, const RunRef&) override {}
};

class Script final : public Model {
public:
    std::function<Decision(const ModelInput&)> decideNext;
    Decision decide(const ModelInput& input) override {
        if (input.event.at("kind") == "intake")
            return {"delegate",{{"instruction","Prepare work"},{"doneWhen","Work is prepared"}},"The work needs a Session"};
        return decideNext(input);
    }
};

struct RecordedTransport final : ChannelTransport {
    std::map<std::string,std::string> sent;
    std::vector<Json> effects;
    int reconciliations = 0;
    bool loseNextSlackReply = true;
    ChannelDelivery send(std::string_view key, const ChannelOrigin& origin, std::string_view action, const Json& payload) override {
        const auto [item,inserted] = sent.emplace(key,"out-"+std::to_string(sent.size()+1));
        check(inserted,"Duplicate outbound call");
        effects.push_back({{"origin",origin.json()},{"action",action},{"payload",payload}});
        if (loseNextSlackReply && origin.kind == ChannelKind::slack && action == "reply") {
            loseNextSlackReply = false;
            return {};
        }
        return {ChannelDelivery::State::accepted,item->second};
    }
    ChannelDelivery reconcile(std::string_view key, const ChannelOrigin&) override {
        ++reconciliations;
        const auto found = sent.find(std::string(key));
        if (found == sent.end()) return {};
        return {ChannelDelivery::State::accepted,found->second};
    }
};

Intake intake(std::string_view bot, std::string_view text) {
    return {std::string(bot),std::string(text),"env","project",{{"modelCallLimit",20}},{},0};
}

void grant(Store& store, std::string_view bot) {
    store.access().personGrant(root,bot,owner.id,Role::decide,GrantChange::grant);
    store.access().resourceGrant(root,bot,{"environment","env","launch"},GrantChange::grant);
}

void connect(Store& store, const std::shared_ptr<RecordedTransport>& transport) {
    const AuthenticateChannel sender = [](auto credential, auto workspace, auto identity) -> std::optional<ChannelSender> {
        if (credential == "fixture-auth" && workspace == "workspace" && identity == "linked-user")
            return ChannelSender{"release",owner.id};
        return std::nullopt;
    };
    store.connectChannel(std::make_shared<SlackConnector>(sender,transport));
    store.connectChannel(std::make_shared<IMessageConnector>(sender,transport,
        ChannelCapabilities{true,false,false,false,{"image/png"},100}));
}

Json event(std::string thread, std::string id, std::string text) {
    return {{"workspaceId","workspace"},{"threadRef",thread},{"externalMessageId",id},{"senderId","linked-user"},{"text",text}};
}

Decision reply(std::string text) {
    return {"reply",{{"text",text},{"suggestions",{"Start here","Show me more"}}},{}};
}

void channels(const std::filesystem::path& database) {
    Script model;
    SessionAdapter t3;
    auto transport = std::make_shared<RecordedTransport>();
    const auto notes = [](auto) { return BotNotes{}; };
    const auto slack = ChannelKind::slack, imessage = ChannelKind::imessage;
    auto screenshot = event("slack-thread","same-id","What is in this screenshot?");
    screenshot["attachments"] = Json::array({{{"name","screenshot.png"},{"mimeType","image/png"},
                                               {"contentBase64",encodeAttachment(image)}}});
    const auto linkedText = event("imessage-thread","same-id","Hello from iMessage");
    Id attachment = 0;
    int calls = 0;
    model.decideNext = [&](const ModelInput& input) {
        ++calls;
        const auto& channel = input.event.at("channel");
        if (channel.at("channelKind") == "slack") {
            const auto& file = input.event.at("attachments").at(0);
            check(file.at("name") == "screenshot.png" && file.at("size") == image.size(),"Missing screenshot metadata");
            attachment = file.at("id");
            check(input.images == Json::array({"data:image/png;base64,"+encodeAttachment(image)}),"Model cannot read screenshot");
        }
        return reply("Thread reply");
    };
    {
        Store store(database);
        connect(store,transport);
        grant(store,"release");
        rejects([&] { store.receiveChannel(slack,"bad",screenshot,intake); });
        rejects([&] { store.receiveChannel(slack,"fixture-auth",screenshot,intake); });
        for (const auto kind : {slack,imessage}) {
            const ChannelOrigin origin{kind,"workspace","",""};
            for (const auto op : {"read","use"}) store.access().resourceGrant(root,"release",channelScope(origin,op),GrantChange::grant);
        }
        const auto a = store.receiveChannel(slack,"fixture-auth",screenshot,intake);
        const auto b = store.receiveChannel(imessage,"fixture-auth",linkedText,intake);
        check(a != b,"Channel keys collided");
        Runtime runtime(store,model,t3,notes);
        runtime.runConversations();
        check(calls == 2 && transport->effects.size() == 2,"Expected one reply in each channel");
        check(store.receiveChannel(slack,"fixture-auth",screenshot,intake) == a &&
              store.receiveChannel(imessage,"fixture-auth",linkedText,intake) == b,"Replay changed receipts");
        auto changed = screenshot;
        changed["attachments"][0]["name"] = "other.png";
        rejects([&] { store.receiveChannel(slack,"fixture-auth",changed,intake); });
        runtime.runConversations();
        check(calls == 2 && transport->effects.size() == 2,"Replay ran model or sent another reply");
        const auto history = store.conversation("release");
        check(history.size() == 4 && history[0]["data"]["channel"]["threadRef"] == "slack-thread",
              "Channel events did not enter conversation history");
        check(store.attachment(owner,attachment)->bytes == image && !store.attachment({"outsider",false},attachment),
              "Attachment missing or exposed to another person");
        for (const auto& effect : transport->effects)
            check(effect["payload"]["suggestions"].size() == 2,"Channel reply lost suggestions");
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));
    Store store(database);
    connect(store,transport);
    Runtime runtime(store,model,t3,notes);
    runtime.runConversations();
    check(transport->reconciliations == 1 && transport->effects.size() == 2,"Lost reply was not reconciled on restart");

    model.decideNext = [&](const ModelInput& input) -> Decision {
        const auto& results = input.event.at("turn");
        for (const auto& entry : results) {
            const auto& result = entry.at("result");
            check(result.at("ok") == false && result.at("unsupported") == true && result.contains("reason") &&
                  !result.contains("delivery"),"Expected explicit unsupported result");
        }
        switch (results.size()) {
        case 0: return {"channel_edit_message",{{"externalMessageId","out-2"},{"text","Edited"}},{}};
        case 1: return {"channel_react",{{"reaction","thumbsup"}},{}};
        case 2: return {"channel_send_attachment",{{"name","large.png"},{"mimeType","image/png"},
                            {"contentBase64",encodeAttachment(image+std::string(100,'x'))}},{}};
        case 3: return {"channel_send_attachment",{{"name","test.txt"},{"mimeType","text/plain"},
                            {"contentBase64",encodeAttachment("test")}},{}};
        case 4: return {"publish_artifact",{{"title","Preview"},{"html","<p>Preview</p>"}},{}};
        default: return reply("Available when you’re ready.");
        }
    };
    const auto unsupported = event("imessage-thread","unsupported","Try the channel actions");
    store.receiveChannel(imessage,"fixture-auth",unsupported,intake);
    runtime.runConversations();
    check(transport->effects.size() == 8,"Expected five limitation notices and a normal reply");
    for (std::size_t i = 2; i < transport->effects.size(); ++i) {
        const auto& effect = transport->effects[i];
        check(effect["action"] == "reply" && effect["origin"]["threadRef"] == "imessage-thread","Unsupported action reached transport");
        if (i < 7) check(effect["payload"]["text"].get<std::string>().find("normal follow-up") != std::string::npos,
                         "No available alternative was offered");
    }
    store.receiveChannel(imessage,"fixture-auth",unsupported,intake);
    runtime.runConversations();
    check(transport->effects.size() == 8,"Unsupported notices were repeated on replay");

    std::string artifact;
    model.decideNext = [&](const ModelInput& input) -> Decision {
        const auto& results = input.event.at("turn");
        switch (results.size()) {
        case 0:
            check(!input.event["channel"]["sentMessages"].empty(),"Reconciled message ID missing");
            return {"channel_edit_message",{{"externalMessageId","out-1"},{"text","Updated"}},{}};
        case 1: return {"channel_react",{{"reaction","thumbsup"}},{}};
        case 2: return {"channel_send_attachment",{{"attachmentId",attachment}},{}};
        case 3: return {"publish_artifact",{{"title","Preview"},{"html","<p>Preview</p>"}},{}};
        default:
            for (const auto& entry : results) check(entry["result"]["delivery"] == "accepted","Channel action was not sent");
            artifact = results.back()["result"]["url"];
            return reply("Actions completed.");
        }
    };
    store.receiveChannel(slack,"fixture-auth",event("slack-thread","actions","Perform the supported actions"),intake);
    runtime.runConversations();
    check(transport->effects.size() == 13 && transport->effects[10]["payload"]["contentBase64"] == encodeAttachment(image),
          "Supported actions or attachment payload missing");
    check(store.artifact(artifact.substr(artifact.find_last_of('/')+1))->bytes == "<p>Preview</p>","Artifact persistence");
    store.access().resourceGrant(root,"release",channelScope({slack,"workspace","",""},"read"),GrantChange::revoke);
    check(!store.attachment(owner,attachment),"Attachment download ignored revocation");
    bool denied = false;
    model.decideNext = [&](const ModelInput& input) {
        const auto& results = input.event.at("turn");
        if (results.empty()) return Decision{"channel_send_attachment",{{"attachmentId",attachment}},{}};
        denied = results[0]["result"]["ok"] == false;
        return reply("The screenshot is unavailable.");
    };
    store.receiveChannel(slack,"fixture-auth",event("slack-thread","revoked","Forward the screenshot"),intake);
    runtime.runConversations();
    check(denied && transport->effects.size() == 14,"Bot attachment reference ignored revocation");
    check(ChannelOrigin{slack,"a:b","c","d"}.key() != ChannelOrigin{slack,"a","b:c","d"}.key(),"Dedupe delimiters collide");
}

void local(const std::filesystem::path& database, const std::string& path) {
    Store store(database);
    grant(store,"release");
    grant(store,"peer");
    store.connectLocalMachine("laptop",hostLocalMachine(owner.id));
    Script model;
    SessionAdapter t3;
    Runtime runtime(store,model,t3,[](auto) { return BotNotes{}; });
    int project = 0;
    const auto session = [&](auto bot) {
        auto route = intake(bot,"Prepare");
        route.projectId += std::to_string(++project);
        const auto task = store.submit(route);
        for (int i = 0; i < 3; ++i) runtime.tick();
        auto run = store.task(task).at("activeRun");
        check(run.is_object(),"Fixture Session did not start");
        return run;
    };
    const auto a = session("release"), b = session("release"), peer = session("peer");
    const auto args = [&](const Json& run) {
        return Json{{"threadId",run.at("threadId")},{"runId",run.at("runId")},{"machineId","laptop"}};
    };
    const auto request = [&](const Json& run) {
        auto data = args(run);
        data["rules"] = Json::array({{{"operation","read_file"},{"path",path}}});
        return Decision{"request_local_machine_connection",data,{}};
    };
    const auto operation = [&](const Json& run, std::string op, std::string file) {
        auto data = args(run);
        data.update(Json{{"operation",op},{"path",file}});
        return Decision{"run_local_machine_operation",data,{}};
    };
    unsigned count = 0;
    const auto turn = [&](auto bot, const Decision& action, bool success, std::string expected = "") {
        bool observed = false;
        model.decideNext = [&](const ModelInput& input) {
            const auto& steps = input.event.at("turn");
            if (steps.empty()) return action;
            const auto& result = steps.at(0).at("result");
            check(result.at("ok") == success,"Unexpected local operation authorization");
            if (!expected.empty()) check(result.at("output").at("text") == expected,"Wrong machine data");
            observed = true;
            return reply("Chat is available");
        };
        store.receive(intake(bot,""),std::to_string(++count),"Local connection");
        runtime.runConversations();
        check(observed,"Local tool result was not delivered");
    };
    const auto environments = store.access().grants("release");
    turn("release",request(a),true);
    const auto id = store.localConnections(owner).at(0).at("id").get<Id>();
    rejects([&] { store.decideLocalConnection(root,id,true); });
    turn("release",operation(a,"read_file",path),false);
    store.decideLocalConnection(owner,id,true);
    turn("release",operation(a,"read_file",path),true,"fixture text");
    turn("release",operation(a,"read_file",path+".forbidden"),false);
    turn("release",operation(a,"list_directory",path),false);
    turn("release",operation(b,"read_file",path),false);
    turn("peer",operation(a,"read_file",path),false);
    turn("peer",operation(peer,"read_file",path),false);
    store.disconnectLocalConnection(owner,id);
    turn("release",operation(a,"read_file",path),false);
    turn("release",request(b),true);
    store.decideLocalConnection(owner,store.localConnections(owner).at(0).at("id"),false);
    turn("release",operation(b,"read_file",path),false);
    check(store.access().grants("release") == environments,"Local approval created reusable grants");
    bool unchanged = false;
    model.decideNext = [&](const ModelInput& input) {
        const auto& steps = input.event.at("turn");
        if (steps.empty()) return Decision{"list_connections",Json::object(),{}};
        unchanged = steps[0]["result"]["environments"] == Json::array({{{"environmentId","env"}}});
        return reply("Chat is available");
    };
    store.receive(intake("release",""),std::to_string(++count),"List reusable environments");
    runtime.runConversations();
    check(unchanged,"Local connections added a reusable environment");
    check(store.conversation("release").back()["data"]["text"] == "Chat is available","Chat disabled after local decline");
}
}

int main() {
    try {
        Temp temp;
        const auto path = temp.path / "notes";
        std::ofstream(path) << "fixture text";
        channels(temp.path / "channels.db");
        local(temp.path / "local.db",std::filesystem::canonical(path).string());
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
