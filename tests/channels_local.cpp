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
    std::function<Observation(const RunRef&)> observeRun;
    std::vector<SessionTarget> evidenceReads;
    std::string authGeneration(const std::string&, const Json&) override { return "auth"; }
    Delivery send(const Command& command) override {
        ++launches;
        const auto thread = command.tool == "follow_up" ? command.target.threadId : "thread-"+std::to_string(launches);
        return {Delivery::Kind::Accepted,{thread,"run-"+std::to_string(launches),""},{}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef& run) override {
        return observeRun ? observeRun(run) : Observation{};
    }
    void interrupt(const std::string&, const Json&, const RunRef&) override {}
    std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool) override {
        evidenceReads.push_back(target);
        return SessionMetadata{target.threadId,"Project","project","running",{},{},{},false};
    }
    std::optional<SessionCounts> sessionCounts(const SessionTarget& target) override {
        evidenceReads.push_back(target);
        return SessionCounts{2,1};
    }
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
        effects.push_back({{"origin",origin.json()},{"action",action},{"payload",payload},{"externalMessageId",item->second}});
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
    return {std::string(bot),std::string(text),"env","project",{{"modelCallLimit",20}}};
}

void grant(Store& store, std::string_view bot) {
    store.access().personGrant(root,bot,owner.id,Role::decide,GrantChange::grant);
    store.access().resourceGrant(root,bot,{"environment","env","launch"},GrantChange::grant);
}

void connect(Store& store, const std::shared_ptr<RecordedTransport>& transport) {
    const auto authenticate = [](auto credential,auto workspace) {
        return credential == "fixture-auth" && workspace == "workspace";
    };
    const auto sender = [](auto workspace, auto identity) -> std::optional<ChannelSender> {
        if (workspace == "workspace" && identity == "linked-user")
            return ChannelSender{"release",owner.id};
        return std::nullopt;
    };
    store.connectChannel({ChannelKind::slack,channelCapabilities(ChannelKind::slack),authenticate,sender,transport});
    store.connectChannel({ChannelKind::imessage,{true,false,false,false,{"image/png"},100},authenticate,sender,transport});
}

Json event(std::string thread, std::string id, std::string text) {
    return {{"workspaceId","workspace"},{"threadRef",thread},{"externalMessageId",id},{"senderId","linked-user"},{"text",text}};
}

Decision reply(std::string text) {
    return {"reply",{{"text",text},{"suggestions",{"Start here","Show me more"}}},{}};
}

Decision answerQuestion(const Json& task, std::string_view text) {
    return {"answer_task_question",{{"taskId",task.at("id")},{"waitId",task.at("wait").at("id")},
        {"revisionId",task.at("currentRevision")},{"generation",task.at("generation")},{"text",text}},{}};
}

Observation finished() {
    Observation result;
    result.kind = Observation::Kind::Finished;
    result.settled = true;
    result.status = "completed";
    result.report = "Session report";
    return result;
}

void channels(const std::filesystem::path& database) {
    Script model;
    SessionAdapter t3;
    auto transport = std::make_shared<RecordedTransport>();
    const auto notes = [](auto) { return std::string{}; };
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
            check(input.images == std::vector<std::string>{"data:image/png;base64,"+encodeAttachment(image)},"Model cannot read screenshot");
        } else {
            check(input.history.empty() && !input.event.contains("earlierAttachments"),"Slack content leaked into iMessage");
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
    store.artifactBaseUrl("https://previews.example.com");
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

    std::string artifact, attachmentPost, artifactPost;
    model.decideNext = [&](const ModelInput& input) -> Decision {
        const auto& results = input.event.at("turn");
        switch (results.size()) {
        case 0:
            check(!input.event["channel"]["sentMessages"].empty(),"Reconciled message ID missing");
            return {"channel_edit_message",{{"externalMessageId","out-1"},{"text","Updated"}},{}};
        case 1: return {"channel_react",{{"reaction","thumbsup"}},{}};
        case 2: return {"channel_send_attachment",{{"attachmentId",attachment}},{}};
        case 3: return {"publish_artifact",{{"title","Preview"},{"html","<p>Preview</p>"}},{}};
        default: {
            for (const auto& entry : results) check(entry["result"]["delivery"] == "accepted","Channel action was not sent");
            artifact = results.back()["result"]["url"];
            attachmentPost = results.at(2).at("result").at("externalMessageId");
            artifactPost = results.at(3).at("result").at("externalMessageId");
            const auto& sent = input.event.at("channel").at("sentMessages");
            check(sent.back().at("text") == "Updated","Accepted edit is missing from sent-message context");
            return reply("Actions completed.");
        }
        }
    };
    store.receiveChannel(slack,"fixture-auth",event("slack-thread","actions","Perform the supported actions"),intake);
    runtime.runConversations();
    check(transport->effects.size() == 13 && transport->effects[10]["payload"]["contentBase64"] == encodeAttachment(image),
          "Supported actions or attachment payload missing");
    const auto artifactId = artifact.substr(artifact.find_last_of('/')+1);
    check(artifact.starts_with("https://previews.example.com/") && store.artifact(artifactId)->bytes == "<p>Preview</p>",
          "Artifact publication needs an absolute URL and persisted HTML");
    model.decideNext = [&](const ModelInput& input) -> Decision {
        const auto& earlier = input.event.at("earlierAttachments");
        const auto& results = input.event.at("turn");
        check(earlier.at(0).at("id") == attachment,"Earlier screenshot was not discoverable in-thread");
        switch (results.size()) {
        case 0: {
            const auto& sent = input.event.at("channel").at("sentMessages");
            for (const auto& id : {attachmentPost,artifactPost})
                check(std::ranges::any_of(sent,[&](const Json& post) { return post.at("externalMessageId") == id; }),
                      "Accepted non-reply post is missing from sent-message context");
            return {"channel_edit_message",{{"externalMessageId",attachmentPost},{"text","Updated attachment"}},{}};
        }
        case 1:
            check(results.at(0).at("result").at("delivery") == "accepted","Attachment post edit was rejected");
            return {"channel_edit_message",{{"externalMessageId",artifactPost},{"text","Updated preview"}},{}};
        case 2:
            check(results.at(1).at("result").at("delivery") == "accepted","Artifact post edit was rejected");
            check(input.images.empty(),"Earlier screenshot was injected without selecting it");
            return {"read_channel_attachment",{{"attachmentId",attachment}},{}};
        default:
            check(input.images == std::vector<std::string>{"data:image/png;base64,"+encodeAttachment(image)} &&
                  results.at(2).at("result").at("imageIncluded") == true,"Earlier screenshot did not reach the model");
            return reply("I can still see the screenshot.");
        }
    };
    store.receiveChannel(slack,"fixture-auth",event("slack-thread","followup","Look at the screenshot again"),intake);
    runtime.runConversations();
    check(transport->effects.size() == 16,"Non-reply edits or screenshot follow-up did not reach the originating thread");
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
    check(denied && transport->effects.size() == 17,"Bot attachment reference ignored revocation");
    store.access().resourceGrant(root,"release",channelScope({slack,"workspace","",""},"use"),GrantChange::revoke);
    check(!store.artifact(artifactId),"Public preview survived source channel revocation");
    check(ChannelOrigin{slack,"a:b","c","d"}.key() != ChannelOrigin{slack,"a","b:c","d"}.key(),"Dedupe delimiters collide");
}

void taskUpdates(const std::filesystem::path& database) {
    Store store(database);
    SessionAdapter t3;
    Script model;
    auto transport = std::make_shared<RecordedTransport>();
    transport->loseNextSlackReply = false;
    connect(store,transport);
    grant(store,"release");
    const auto channel = ChannelKind::slack;
    store.access().resourceGrant(root,"release",channelScope({channel,"workspace","",""},"use"),GrantChange::grant);
    Runtime runtime(store,model,t3,[](auto) { return std::string{}; });
    auto original = event("private-origin","start","Start the private release task");
    auto publish = event("private-origin","publish","Publish the preview");

    model.decideNext = [&](const ModelInput& input) -> Decision {
        if (input.event.at("text") == "Publish the preview") {
            if (input.event.at("turn").empty()) return {"publish_artifact",{{"title","test"},{"html","<p>test</p>"}},{}};
            check(input.event.at("turn").at(0).at("result").at("unsupported") == true,
                  "Missing preview origin wasn't reported as unsupported");
            return reply("Preview unavailable.");
        }
        if (input.event.at("turn").empty()) return {"start_task",{{"request","A private release task"}},{}};
        return reply("Task started.");
    };
    store.receiveChannel(channel,"fixture-auth",publish,intake);
    runtime.runConversations();
    check(transport->effects.size() == 2 && transport->effects.at(0).at("action") == "reply" &&
          transport->effects.at(0).at("payload").at("text").get<std::string>().find("preview URL") != std::string::npos,
          "Publishing without a preview origin reached transport or did not explain the limitation");
    const auto receipt = store.receiveChannel(channel,"fixture-auth",original,intake);
    for (int i = 0; i < 4; ++i) runtime.tick();
    const auto taskId = store.activeTasks("release").at(0).at("id").get<Id>();
    const auto active = store.task(taskId).at("activeRun");
    check(active.is_object(),"Channel task did not start a Session");
    t3.observeRun = [](const RunRef&) {
        Observation observed;
        observed.kind = Observation::Kind::Question;
        observed.question.requestId = "release-question";
        return observed;
    };
    model.decideNext = [](const ModelInput& input) -> Decision {
        if (input.event.at("kind") == "question")
            return {"ask_user",{{"question","Which private target?"},{"choices",{"Preview","Production"}}},
                    "The release needs a target"};
        return reply("Unexpected turn");
    };
    for (int i = 0; i < 4; ++i) runtime.tick();
    check(transport->effects.size() == 4 && transport->effects.back().at("origin").at("threadRef") == "private-origin" &&
          transport->effects.back().at("payload").at("suggestions") == Json::array({"Preview","Production"}),
          "Task question was not delivered to the originating thread");

    model.decideNext = [&](const ModelInput& input) -> Decision {
        check(input.task.at("tasks").empty() && input.history.empty(),"Another thread received private task context");
        const auto& steps = input.event.at("turn");
        if (steps.empty()) return {"cancel_task",{{"taskId",taskId},{"generation",store.task(taskId).at("generation")}},{}};
        check(steps.at(0).at("result").at("ok") == false,"Another thread could address a private task by ID");
        return reply("No task in this thread.");
    };
    store.receiveChannel(channel,"fixture-auth",event("other-thread","ask","What is the release target?"),intake);
    runtime.runConversations();
    check(store.task(taskId).at("state") == "waiting","Another thread cancelled a private task");

    model.decideNext = [&](const ModelInput& input) -> Decision {
        if (input.event.at("kind") == "result")
            return {"complete",{{"report","Private release completed."}},"The Session confirmed the release"};
        const auto& steps = input.event.at("turn");
        if (steps.empty()) return answerQuestion(store.task(taskId),"Preview");
        check(steps.at(0).at("result").at("ok") == true,"Originating thread could not answer its question");
        return reply("Answer received.");
    };
    const auto answer = event("private-origin","answer","Preview");
    store.receiveChannel(channel,"fixture-auth",answer,intake);
    runtime.runConversations();
    check(store.decisionHistory(root).at(0).at("decidedBy") == owner.id,"Channel decision lost the verified sender");
    t3.observeRun = [](const RunRef&) { return finished(); };
    for (int i = 0; i < 5; ++i) runtime.tick();
    check(store.task(taskId).at("state") == "completed" && transport->effects.size() == 7 &&
          transport->effects.back().at("origin").at("threadRef") == "private-origin" &&
          transport->effects.back().at("payload").at("text") == "Private release completed.",
          "Task completion was not delivered to the originating thread");
    check(store.receiveChannel(channel,"fixture-auth",original,intake) == receipt,"Task-start event changed on replay");
    store.receiveChannel(channel,"fixture-auth",answer,intake);
    runtime.runConversations();
    check(transport->effects.size() == 7,"Task replay repeated a channel update");
}

void sessionEvidence(const std::filesystem::path& database) {
    Store store(database);
    SessionAdapter t3;
    Script model;
    auto transport = std::make_shared<RecordedTransport>();
    transport->loseNextSlackReply = false;
    connect(store,transport);
    grant(store,"release");
    const auto channel = ChannelKind::slack;
    store.access().resourceGrant(root,"release",channelScope({channel,"workspace","",""},"use"),GrantChange::grant);
    store.access().resourceGrant(root,"release",Scope::sessionRead("env","shared-from-web"),GrantChange::grant);
    Runtime runtime(store,model,t3,[](auto) { return std::string{}; });
    model.decideNext = [](const ModelInput& input) -> Decision {
        if (input.event.at("turn").empty()) return {"start_task",{{"request","Private task"}},{}};
        return reply("Started");
    };
    store.receiveChannel(channel,"fixture-auth",event("private-origin","start","Start work"),intake);
    for (int i = 0; i < 4; ++i) runtime.tick();
    check(t3.launches == 1,"Expected a Session in the originating thread");

    Json ref, result;
    model.decideNext = [&](const ModelInput& input) -> Decision {
        const auto& steps = input.event.at("turn");
        if (!steps.empty()) {
            result = steps.back().at("result");
            return reply("Checked");
        }
        if (input.event.at("text") == "Count events")
            return {"flock_session_events",{{"sessionRef",ref},{"operation","counts"}},{}};
        if (input.event.at("text") == "Read memory") {
            check(std::ranges::find(input.tools,"read_memory") == input.tools.end() &&
                  std::ranges::find(input.tools,"create_schedule") == input.tools.end(),
                  "Bot-wide memory or scheduling was offered in a channel thread");
            return {"read_memory",Json::object(),{}};
        }
        return {"flock_session_search",Json::object(),{}};
    };
    const auto call = [&](std::string thread, std::string id, std::string request) {
        result = nullptr;
        store.receiveChannel(channel,"fixture-auth",event(std::move(thread),std::move(id),std::move(request)),intake);
        runtime.runConversations();
        check(result.is_object(),"Expected Session evidence tool result");
        return result;
    };
    auto found = call("private-origin","search","Search Sessions");
    check(found.at("sessions").size() == 1 && found.at("sessions").at(0).at("title") == "thread-1",
          "Originating thread did not see exactly its own Session");
    ref = found.at("sessions").at(0).at("sessionRef");
    check(call("private-origin","count","Count events").at("counts").at("runs") == 1,
          "Originating thread could not read its Session");
    t3.evidenceReads.clear();
    check(call("other-thread","search","Search Sessions").at("sessions").empty(),"Session search crossed channel threads");
    check(call("other-thread","count","Count events").at("ok") == false && t3.evidenceReads.empty(),
          "A Session reference bypassed channel scope");
    check(call("private-origin","memory","Read memory").at("ok") == false,
          "An unlisted tool could access Bot-wide memory in a channel thread");
}

void taskQuestionChoices(const std::filesystem::path& database) {
    Store store(database);
    SessionAdapter t3;
    Script model;
    auto transport = std::make_shared<RecordedTransport>();
    transport->loseNextSlackReply = false;
    connect(store,transport);
    grant(store,"release");
    const auto channel = ChannelKind::slack;
    store.access().resourceGrant(root,"release",channelScope({channel,"workspace","",""},"use"),GrantChange::grant);
    Runtime runtime(store,model,t3,[](auto) { return std::string{}; });
    t3.observeRun = [](const RunRef& run) {
        Observation observed;
        observed.kind = Observation::Kind::Question;
        observed.question.requestId = "question-" + run.runId;
        return observed;
    };
    model.decideNext = [](const ModelInput& input) -> Decision {
        if (input.event.at("kind") == "question")
            return {"ask_user",{{"question","Continue?"},{"choices",{"Yes","No"}}},"The Session needs approval to continue"};
        if (input.event.at("turn").empty()) return {"start_task",{{"request",input.event.at("text")}},{}};
        return reply("Task started.");
    };
    const auto start = [&](const std::string& project) {
        store.receiveChannel(channel,"fixture-auth",event("choice-thread",project,project),
            [&](std::string_view bot, std::string_view text) {
                auto route = intake(bot,text);
                route.projectId = project;
                return route;
            });
        for (int i = 0; i < 8; ++i) runtime.tick();
        const auto task = store.activeTasks("release").at(0);
        check(task.at("state") == "waiting" && transport->effects.back().at("payload").at("suggestions") == Json::array({"Yes","No"}),
              "The task question was not posted");
        return std::pair{task,transport->effects.back().at("externalMessageId")};
    };
    const auto [first,firstPost] = start("first");
    const auto [second,secondPost] = start("second");
    check(first.at("id") != second.at("id") && firstPost != secondPost && store.task(first.at("id")).at("state") == "waiting",
          "Both questions must be open at the same time");
    auto answer = event("choice-thread","answer-first","Yes");
    answer["replyToExternalMessageId"] = firstPost;
    auto wrongThread = answer;
    wrongThread["threadRef"] = "another-thread";
    rejects([&] { store.receiveChannel(channel,"fixture-auth",wrongThread,intake); });

    bool answered = false;
    model.decideNext = [&](const ModelInput& input) -> Decision {
        const auto& anchor = input.event.at("replyTo");
        check(anchor.at("taskId") == first.at("id") && anchor.at("messageId") == first.at("wait").at("messageId") &&
              anchor.at("waitId") == first.at("wait").at("id"),"Posted choice lost its exact journal target");
        const auto& turn = input.event.at("turn");
        if (turn.empty()) return answerQuestion(second,"Yes");
        check(turn.at(0).at("result").at("ok") == false,"Selection was applied to the other open question");
        if (turn.size() == 1) return answerQuestion(first,"Yes");
        answered = turn.at(1).at("result").at("ok") == true;
        return reply("Answer received.");
    };
    const auto receipt = store.receiveChannel(channel,"fixture-auth",answer,intake);
    runtime.runConversations();
    check(answered && store.task(second.at("id")).at("state") == "waiting","Only the selected question should be answered");
    check(store.receiveChannel(channel,"fixture-auth",answer,intake) == receipt,"Choice replay changed the receipt");
    answer["replyToExternalMessageId"] = secondPost;
    rejects([&] { store.receiveChannel(channel,"fixture-auth",answer,intake); });

    auto clarification = event("choice-thread","clarify-second","What happens if I say yes?");
    clarification["replyToExternalMessageId"] = secondPost;
    model.decideNext = [](const ModelInput&) { return reply("The Session continues. Shall it proceed?"); };
    store.receiveChannel(channel,"fixture-auth",clarification,intake);
    runtime.runConversations();
    auto followup = event("choice-thread","answer-clarification","Yes");
    followup["replyToExternalMessageId"] = transport->effects.back().at("externalMessageId");

    bool followed = false;
    model.decideNext = [&](const ModelInput& input) -> Decision {
        const auto& anchor = input.event.at("replyTo");
        check(anchor.at("kind") == "bot_message" &&
              anchor.at("inReplyTo").at("replyToMessageId") == second.at("wait").at("messageId"),
              "Clarification reply lost the channel question it followed");
        const auto& turn = input.event.at("turn");
        if (turn.empty()) return answerQuestion(second,"Yes");
        followed = turn.at(0).at("result").at("ok") == true;
        return reply("Answer received.");
    };
    store.receiveChannel(channel,"fixture-auth",followup,intake);
    runtime.runConversations();
    check(followed && store.task(second.at("id")).at("state") != "waiting",
          "Reply to a Bot clarification could not answer its original question");
}

void local(const std::filesystem::path& database, const std::string& path) {
    Store store(database);
    grant(store,"release");
    grant(store,"peer");
    store.connectLocalMachine("laptop",hostLocalMachine(owner.id));
    Script model;
    SessionAdapter t3;
    Runtime runtime(store,model,t3,[](auto) { return std::string{}; });
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
    auto a = session("release");
    const auto b = session("release"), peer = session("peer");
    const auto args = [&](const Json& run) {
        return Json{{"threadId",run.at("threadId")},{"runId",run.at("runId")},{"machineId","laptop"}};
    };
    const auto request = [&](const Json& run) {
        auto data = args(run);
        data["rules"] = Json::array({{{"operation","read_file"},{"path",path}}});
        return Decision{"request_local_machine_connection",data,{}};
    };
    const auto operation = [&](const Json& run, std::string_view op, const std::string& file) {
        const RunRef ref{run.at("threadId"),run.at("runId"),""};
        return store.runLocalMachineOperation("env",ref,"laptop",op,file);
    };
    unsigned count = 0;
    const auto turn = [&](auto bot, const Decision& action, bool success) {
        bool observed = false;
        model.decideNext = [&](const ModelInput& input) {
            const auto& steps = input.event.at("turn");
            if (steps.empty()) return action;
            const auto& result = steps.at(0).at("result");
            check(result.at("ok") == success,"Unexpected local operation authorization");
            observed = true;
            return reply("Chat is available");
        };
        store.receive(localOwner,intake(bot,""),std::to_string(++count),"Local connection");
        runtime.runConversations();
        check(observed,"Local tool result was not delivered");
    };
    const auto environments = store.access().grants("release");
    turn("release",request(a),true);
    const auto id = store.localConnections(owner).at(0).at("id").get<Id>();
    rejects([&] { store.decideLocalConnection(root,id,true); });
    rejects([&] { operation(a,"read_file",path); });
    const auto originalRun = a;
    const auto originalRunId = originalRun.at("runId").get<std::string>();
    t3.observeRun = [&](const RunRef& run) { return run.runId == originalRunId ? finished() : Observation{}; };
    model.decideNext = [](const ModelInput&) {
        return Decision{"follow_up",{{"instruction","Continue in this Session"}},"The work needs one more Run"};
    };
    for (int i = 0; i < 5; ++i) runtime.tick();
    for (const auto& task : store.activeTasks("release")) {
        const auto active = store.task(task.at("id")).at("activeRun");
        if (active.is_object() && active.at("threadId") == originalRun.at("threadId")) a = active;
    }
    check(a.at("runId") != originalRun.at("runId"),"Fixture did not start a follow-up Run in Session A");
    store.decideLocalConnection(owner,id,true);
    rejects([&] { operation(originalRun,"read_file",path); });
    check(operation(a,"read_file",path).at("text") == "fixture text","Wrong machine data");
    rejects([&] { operation(a,"read_file",path+".forbidden"); });
    rejects([&] { operation(a,"list_directory",path); });
    rejects([&] { operation(b,"read_file",path); });
    rejects([&] { operation(peer,"read_file",path); });
    turn("release",{"run_local_machine_operation",args(a),{}},false);
    const auto original = path + ".original";
    std::filesystem::rename(path,original);
    std::filesystem::create_symlink(original,path);
    rejects([&] { operation(a,"read_file",path); });
    std::filesystem::remove(path);
    std::filesystem::rename(original,path);
    store.disconnectLocalConnection(owner,id);
    rejects([&] { operation(a,"read_file",path); });
    turn("release",request(b),true);
    store.decideLocalConnection(owner,store.localConnections(owner).at(0).at("id"),false);
    rejects([&] { operation(b,"read_file",path); });
    check(store.access().grants("release") == environments,"Local approval created reusable grants");
    bool unchanged = false;
    model.decideNext = [&](const ModelInput& input) {
        const auto& steps = input.event.at("turn");
        if (steps.empty()) return Decision{"list_connections",Json::object(),{}};
        unchanged = steps[0]["result"]["environments"] == Json::array({{{"environmentId","env"}}});
        return reply("Chat is available");
    };
    store.receive(localOwner,intake("release",""),std::to_string(++count),"List reusable environments");
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
        taskUpdates(temp.path / "task-updates.db");
        sessionEvidence(temp.path / "session-evidence.db");
        taskQuestionChoices(temp.path / "task-question-choices.db");
        local(temp.path / "local.db",std::filesystem::canonical(path).string());
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
