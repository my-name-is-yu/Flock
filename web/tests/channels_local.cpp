#include "flock/task_list.hpp"
#include "http_fixture.hpp"
#include "flock/identity.hpp"
#include "flock/channels.hpp"
#include "flock/decision.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0::http_test {
void channelEndpoints(const std::filesystem::path& database) {
    struct Session final : T3 {
        std::string authGeneration(const std::string&, const Json&) override { return "fixture-auth"; }
        Delivery send(const Command&) override { return {Delivery::Kind::Accepted, {"thread-local", "run-local", ""}, {}}; }
        Delivery reconcile(const Command&) override { return {}; }
        Observation observe(const std::string&, const Json&, const RunRef&) override { return {}; }
        InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { return InterruptOutcome::Requested; }
    };
    struct Transport final : ChannelTransport {
        std::string preview;
        std::vector<Json> replies;
        ChannelDelivery send(std::string_view key, const ChannelOrigin& origin, std::string_view operation, const Json& payload) override {
            if (operation == "publish_artifact") {
                const auto url = payload.at("url").get<std::string>();
                check(url.starts_with("https://previews.example.com/"), "Preview publication is missing its configured origin");
                preview = url.substr(std::string_view("https://previews.example.com").size());
            }
            if (operation == "reply") replies.push_back({{"origin", origin.json()}, {"payload", payload}});
            return {ChannelDelivery::State::accepted, std::string(key)};
        }
        ChannelDelivery reconcile(std::string_view, const ChannelOrigin&) override { return {}; }
    };
    struct ChannelModel final : Model {
        Json local, review;
        ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
        Decision choose(const ModelInput& input) {
            if (input.event["kind"] == "intake")
                return {"delegate", {{"instruction", "Prepare"}, {"doneWhen", "Prepared"}}, "The work needs a Session"};
            const auto& turn = input.event.at("turn");
            if (input.event.at("text") == "Request local") {
                check(input.task.at("localMachines") == Json::array({"fixture"}), "Configured root owner's machine is missing");
                if (turn.empty()) return {"request_local_machine_connection", local, {}};
                review = turn.at(0).at("result");
            }
            if (turn.empty()) {
                if (input.event.at("text") == "Prepare session") return {"start_task", {{"request", "Prepare"}}, {}};
                if (input.event["channel"]["channelKind"] == "slack")
                    return {"publish_artifact", {{"title", "Show preview"}, {"html", "<p>Preview</p>"}}, {}};
            }
            return {"reply", {{"text", "Thread reply"}, {"suggestions", {"Get started"}}}, {}};
        }
    };
    Store store(database);
    const Person owner{"admin", true}, sender{"alice"};
    Policy{store}.setMember(owner, "release", sender.id, true);
    const auto transport = std::make_shared<Transport>();
    const auto authenticate = [](auto credential, auto workspace) { return credential == "Bearer fixture-channel" && workspace == "W"; };
    const auto authorize = [&](auto workspace, auto user) -> std::optional<ChannelSender> {
        if (workspace == "W" && user == "U") return ChannelSender{"release", sender.id};
        return std::nullopt;
    };
    for (const auto channel : {ChannelKind::slack, ChannelKind::imessage})
        Channels(store).connectChannel({channel, channelCapabilities(channel), authenticate, authorize, transport});
    WebCallbacks cb{
        .bots = [] { return Json::array({{{"id", "release"}, {"name", "Release"}}}); },
        .intake = [](std::string_view bot, std::string_view request) -> Intake {
            return {std::string(bot), std::string(request), "local-env", "release", Json::object()};
        },
    };
    const auto identities = database.parent_path() / "channel-identities.json";
    std::ofstream(identities) << Json::array({{{"authorization", "Bearer fixture-owner"}, {"person", owner.id}, {"adminRoot", true}},
                                              {{"authorization", "Bearer fixture-owner-member"}, {"person", owner.id}},
                                              {{"authorization", "Bearer fixture-sender"}, {"person", sender.id}}});
    std::filesystem::permissions(identities, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    configureWebIdentities(store, identities, cb);
    check(cb.resolveIdentity(owner.id) == false, "The fixture owner must have a conservative queued role");
    check(!Policy{store}.allows(Person{owner.id}, Action::bot_decide, "release"),
          "Machine owner unexpectedly has permission to decide for the Bot");
    const std::string sessionCredential(32, 's');
    const auto config = database.parent_path() / "session-bridge.json";
    std::ofstream(config) << Json{{"localMachines", {{"fixture", owner.id}}},
                                  {"sessionBridgeCredentialEnv", "FLOCK_HTTP_TEST_SESSION_KEY"},
                                  {"artifactBaseUrl", "https://previews.example.com"}}
                                 .dump();
    setenv("FLOCK_HTTP_TEST_SESSION_KEY", sessionCredential.c_str(), 1);
    configureChannelBridges(store, config, cb);
    unsetenv("FLOCK_HTTP_TEST_SESSION_KEY");
    auto server = startWeb(store, 0, cb);
    Browser browser(*server);
    httplib::Client bridge("127.0.0.1", static_cast<int>(server->port()));
    const std::string slack = "/api/channels/slack/events", imessage = "/api/channels/imessage/events";
    Json event{{"workspaceId", "W"}, {"threadRef", "thread"}, {"externalMessageId", "id"}, {"senderId", "U"}, {"text", "Preview"}};
    const auto image = decodeAttachment("iVBORw0KGgo=", 8);
    event["attachments"] = Json::array({{{"name", "image.png"}, {"mimeType", "image/png"}, {"contentBase64", encodeAttachment(image)}}});
    Browser::checked(bridge.Post(slack, event.dump(), "application/json"), 401);
    const httplib::Headers credential{{"Authorization", "Bearer fixture-channel"}};
    auto spoofed = event;
    spoofed["senderId"] = "unknown";
    Browser::checked(bridge.Post(slack, credential, spoofed.dump(), "application/json"), 403);
    const auto a = Browser::checked(bridge.Post(slack, credential, event.dump(), "application/json"), 201);
    event.erase("attachments");
    const auto b = Browser::checked(bridge.Post(imessage, credential, event.dump(), "application/json"), 201);
    check(a != b, "HTTP channels collided");
    check(Browser::checked(bridge.Post(imessage, credential, event.dump(), "application/json"), 201) == b, "HTTP replay changed receipt");
    ChannelModel model;
    Session t3;
    Runtime runtime(store, model, t3, [](auto) { return std::string{}; });
    runtime.runConversations();
    check(transport->replies.size() == 2 && transport->replies[0]["payload"]["suggestions"].size() == 1,
          "Channel replies or getting-started suggestions missing");
    const auto preview = bridge.Get(transport->preview, httplib::Headers{{"Sec-Fetch-Site", "cross-site"}});
    check(preview && preview->status == 200 && preview->body == "<p>Preview</p>" &&
              preview->get_header_value("Content-Type").starts_with("text/html") &&
              preview->get_header_value("Content-Security-Policy").starts_with("sandbox"),
          "Artifact preview missing sandbox or HTML");
    Browser::checked(bridge.Get("/api/artifacts/flock-00000000000000000000000000000000"), 404);
    Browser::checked(bridge.Get("/api/channels/local-connections"), 401);
    browser.client.set_default_headers({{"Authorization", "Bearer fixture-owner"}});
    const auto history = browser.get("/api/conversation?bot=release").at("events");
    check(history.size() == 4 && history.at(0).at("kind") == "conversation_message" && history.at(1).at("kind") == "conversation_message",
          "HTTP conversation does not show channel intake");
    check(history[0]["data"]["channel"] == Json{{"channelKind", "slack"}, {"fromViewer", false}} &&
              history[1]["data"]["channel"] == Json{{"channelKind", "imessage"}, {"fromViewer", false}},
          "HTTP channel provenance must include only the kind and whether the viewer sent it");
    check(browser.get("/api/view?bot=release").at("events") == history,
          "The initial timeline and conversation history must project the same channel provenance");
    browser.client.set_default_headers({{"Authorization", "Bearer fixture-sender"}});
    const auto ownHistory = browser.get("/api/conversation?bot=release").at("events");
    check(ownHistory[0]["data"]["channel"] == Json{{"channelKind", "slack"}, {"fromViewer", true}} &&
              ownHistory[1]["data"]["channel"] == Json{{"channelKind", "imessage"}, {"fromViewer", true}} &&
              browser.get("/api/view?bot=release").at("events") == ownHistory,
          "The signed-in sender did not see their channel messages attributed to themselves");
    browser.client.set_default_headers({{"Authorization", "Bearer fixture-owner"}});
    const auto attachment = history[0]["data"]["attachments"][0]["id"].get<Id>();
    const auto file = browser.client.Get("/api/channels/attachments/" + std::to_string(attachment));
    check(file && file->status == 200 && file->body == image, "HTTP screenshot download missing");
    server.reset();
    event["text"] = "Prepare session";
    event["externalMessageId"] = "prepare-local";
    Channels(store).receiveChannel(ChannelKind::slack, "Bearer fixture-channel", event, cb.intake);
    for (int i = 0; i < 4; ++i) runtime.tick();
    const auto active = TaskList(store).activeTasks("release").at(0).task.activeRun;
    check(active.has_value(), "Fixture Session did not start");
    const auto localFile = std::filesystem::canonical(database.parent_path()) / "session-local.txt";
    std::ofstream(localFile) << "Approved only for this Session";
    model.local = {{"threadId", active->ref.threadId},
                   {"runId", active->ref.runId},
                   {"machineId", "fixture"},
                   {"rules", Json::array({{{"operation", "read_file"}, {"path", localFile.string()}}})}};
    event["text"] = "Request local";
    event["externalMessageId"] = "local-headless";
    Channels(store).receiveChannel(ChannelKind::slack, "Bearer fixture-channel", event, cb.intake);
    runtime.runConversations();
    check(model.review.at("status") == "pending" && !model.review.contains("reviewUrl") &&
              model.review.at("reviewInstructions").get<std::string>().find("this Bot's Access, and review Connected computers") !=
                  std::string::npos,
          "Headless channel approval needs Web navigation guidance");
    const auto id = model.review.at("connectionId");

    server = startWeb(store, 0, cb);
    Browser reviewBrowser(*server);
    reviewBrowser.client.set_default_headers({{"Authorization", "Bearer fixture-owner"}});
    httplib::Client sessionBridge("127.0.0.1", static_cast<int>(server->port()));
    event["externalMessageId"] = "local-with-web";
    Browser::checked(sessionBridge.Post(slack, credential, event.dump(), "application/json"), 201);
    runtime.runConversations();
    const std::string settings = "/settings/release";
    check(model.review.at("connectionId") == id && model.review.at("reviewUrl") == reviewBrowser.origin + settings + "#local-connections",
          "External approval did not use the bound Web origin for the pending request");
    const httplib::Headers navigation{{"Sec-Fetch-Site", "cross-site"}, {"Sec-Fetch-Mode", "navigate"}, {"Sec-Fetch-Dest", "document"}};
    Browser::checked(sessionBridge.Get(settings, navigation), 401);
    Browser::checked(reviewBrowser.client.Get(settings, httplib::Headers{{"Sec-Fetch-Site", "cross-site"}}), 403);
    const auto page = reviewBrowser.client.Get(settings, navigation);
    check(page && page->status == 200 && page->body.find("id=\"local-connections\"") != std::string::npos,
          "The signed-in machine owner could not follow the approval link from an external channel");
    const auto connections = reviewBrowser.get("/api/channels/local-connections");
    check(connections.size() == 1 && connections[0]["status"] == "pending", "Local request HTTP listing");
    const auto endpoint = "/api/channels/local-operations";
    auto request = model.local;
    request.erase("rules");
    request.update(Json{{"environmentId", "local-env"}, {"operation", "read_file"}, {"path", localFile.string()}});
    const httplib::Headers bridgeIdentity{{"Authorization", "Bearer " + sessionCredential}};
    Browser::checked(sessionBridge.Post(endpoint, credential, request.dump(), "application/json"), 401);
    Browser::checked(sessionBridge.Post(endpoint, bridgeIdentity, request.dump(), "application/json"), 403);
    reviewBrowser.post("/api/channels/local-connections/decide", {{"id", id}, {"approve", true}});
    check(Browser::checked(sessionBridge.Post(endpoint, bridgeIdentity, request.dump(), "application/json"), 200).at("text") ==
              "Approved only for this Session",
          "Approved Session could not read its file through the bridge");
    auto otherRun = request;
    otherRun["runId"] = "different-run";
    Browser::checked(sessionBridge.Post(endpoint, bridgeIdentity, otherRun.dump(), "application/json"), 403);
    reviewBrowser.post("/api/channels/local-connections/disconnect", {{"id", id}});
    Browser::checked(sessionBridge.Post(endpoint, bridgeIdentity, request.dump(), "application/json"), 403);
    check(reviewBrowser.get("/api/channels/local-connections")[0]["status"] == "disconnected", "Local decision endpoints");
    event["externalMessageId"] = "queued-before-pause";
    const auto accepted = Browser::checked(sessionBridge.Post(imessage, credential, event.dump(), "application/json"), 201);
    const auto sentBeforePause = transport->replies.size();
    store.setBotPaused(owner, "release", true);
    check(Browser::checked(sessionBridge.Post(imessage, credential, event.dump(), "application/json"), 201) == accepted,
          "Pause changed an existing channel receipt");
    event["externalMessageId"] = "while-paused";
    for (const auto& path : {slack, imessage}) {
        const auto paused = Browser::checked(sessionBridge.Post(path, credential, event.dump(), "application/json"), 409);
        check(paused.at("error") == botPausedMessage, "Channel did not explain why the Bot refused a message");
    }
    runtime.runConversations();
    store.setBotPaused(owner, "release", false);
    runtime.runConversations();
    check(transport->replies.size() == sentBeforePause, "A channel reply escaped pause or replayed on resume");
    event["externalMessageId"] = "after-resume";
    event["text"] = "New message";
    Browser::checked(sessionBridge.Post(imessage, credential, event.dump(), "application/json"), 201);
    runtime.runConversations();
    check(transport->replies.size() == sentBeforePause + 1, "A resumed Bot could not reply to a new channel message");
    event["externalMessageId"] = "queued-before-revocation";
    Browser::checked(sessionBridge.Post(slack, credential, event.dump(), "application/json"), 201);
    const auto sentBeforeRevocation = transport->replies.size();
    WebIdentity(store).setWebIdentityResolver([&](std::string_view person) -> std::optional<bool> {
        if (person == owner.id) return true;
        return std::nullopt;
    });
    runtime.runConversations();
    check(transport->replies.size() == sentBeforeRevocation, "Revoked sender's queued channel reply was delivered");
    event["externalMessageId"] = "new-after-revocation";
    Browser::checked(sessionBridge.Post(slack, credential, event.dump(), "application/json"), 403);
}
}
