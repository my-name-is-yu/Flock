#include "flock/conversation_intake.hpp"
#include "flock/task_control.hpp"
#include "flock/task_intake.hpp"
#include "flock/identity.hpp"
#include "flock/secret_ref.hpp"
#include "flock/decision_errors.hpp"
#include "flock/tasks.hpp"
#include "flock/automations.hpp"
#include "flock/bot_memory.hpp"
#include "flock/board_inbox.hpp"
#include "flock/channels.hpp"
#include "flock/bot_change.hpp"
#include "flock/decision.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/person.hpp"
#include "flock/bot_access.hpp"
#include "flock/policy.hpp"
#include "flock/policy_rules.hpp"
#include "flock/secret_requests.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/runtime_options.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"
#include "flock/task_target.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <sqlite3.h>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace flockv0;
namespace {
const Person requester{"requester"}, member{"member"};
const Intake intake{"release", "", "env", "project"};

void check(bool value, std::string_view label) {
    if (!value) throw std::runtime_error(std::string(label));
}

template <class Error = Conflict, class F> void rejected(F&& action) {
    try {
        action();
    } catch (const Error&) {
        return;
    }
    throw std::runtime_error("Expected the decision to be rejected");
}

InboxTarget target(const Json& item) {
    return {item.at("botId"),           parseInboxKind(item.at("itemKind").get_ref<const std::string&>()),
            item.at("itemId"),          item.at("waitId"),
            item.at("taskId"),          item.at("targetRevision"),
            item.at("targetGeneration")};
}

struct FixtureModel : Model {
    Decision action;
    bool retry = false;
    Json outcome, channelContext;
    ModelTurn decide(const ModelInput& in) override {
        if (in.event.at("kind") != "conversation_turn") {
            if (in.event.at("kind") == "intake")
                return {{{"delegate", {{"instruction", "Build the release"}, {"doneWhen", "Release built"}}, "Delegate"}}};
            outcome = in.event;
            return {{{"ask_user", {{"question", "The launch did not run. How should I proceed?"}}, "Report the outcome"}}};
        }
        channelContext = in.event.value("channel", Json::object());
        const auto& steps = in.event.at("turn");
        if (steps.empty() || (retry && steps.size() == 1)) return {{action}};
        outcome = steps;
        return {{{"reply", {{"text", "Done"}}, "Reply"}}};
    }
};

struct Adapter : T3 {
    int sends = 0;
    std::string instruction;
    std::string authGeneration(const std::string&, const Json&) override { return "fixture"; }
    Delivery send(const Command& command) override {
        ++sends;
        instruction = command.instruction;
        return {Delivery::Kind::Accepted, {"thread-" + std::to_string(sends), "run", ""}, {}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef&) override { return {}; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { return InterruptOutcome::Requested; }
};

void rules(const std::filesystem::path& path) {
    Store store(path);
    Policy policy{store};
    PolicyRules policyRules{store};
    const auto asks = {Action::repository_write, Action::repository_pr, Action::repository_review,  Action::repository_issues,
                       Action::channel_post,     Action::email_send,    Action::environment_launch, Action::t3_send,
                       Action::bot_create,       Action::bot_rename,    Action::automation_change,  Action::destructive_write,
                       Action::secret_use,       Action::mcp_invoke};
    for (const auto mode : {"full", "supervised"}) {
        policyRules.setMode(localOwner, mode);
        for (const auto action : asks)
            check(policy.check("release", action, "target") == (policy.mode() == "full" ? PolicyResult::allow : PolicyResult::ask),
                  actionName(action));
        for (const auto action : {Action::repository_read, Action::workspace_read, Action::workspace_write, Action::web_search,
                                  Action::web_fetch, Action::skill_list, Action::skill_read, Action::mcp_discover,
                                  Action::mcp_read_resource, Action::session_read, Action::attachment_inspect, Action::shared_memory_read})
            check(policy.allows("release", action, "target"), actionName(action));
    }
    for (const auto action : {Action::t3_send, Action::secret_use, Action::mcp_invoke})
        check(policy.check("release", Resource{action, "target", true}) == PolicyResult::allow, "routine action asked");
    const auto action = Action::repository_pr;
    policyRules.changeRule(localOwner, RuleKind::allow, action, "github.com/acme/*", true);
    check(policy.allows("release", action, "github.com/acme/release"), "Allow glob did not match");
    policyRules.changeRule(localOwner, RuleKind::ask, action, "github.com/*/release", true);
    check(policy.check("release", action, "github.com/acme/release") == PolicyResult::ask, "Ask first did not override Allow");
    policyRules.changeRule(localOwner, RuleKind::never, action, "github.com/acme/release", true);
    check(policy.forbidden("release", action, "github.com/acme/release"), "Never did not override Ask first");
    policyRules.setMode(localOwner, "full");
    check(policy.check("release", action, "github.com/acme/release") == PolicyResult::deny &&
              policy.check("release", action, "github.com/another/release") == PolicyResult::allow,
          "Full must enforce Never and ignore Ask first");
    rejected([&] { policyRules.setMode(requester, "supervised"); });
}

void reopen(const std::filesystem::path& path) {
    Id oldMessage;
    {
        Store store(path);
        BotAccess{store}.setOwner(requester, "release");
        PolicyRules{store}.changeNeverRule(localOwner, Action::repository_read, "github.com/example/private", true);
        oldMessage = ConversationIntake(store).receive(requester, intake, "existing", "Existing turn").at("messageId");
    }
    Store reopened(path);
    Policy policy{reopened};
    PolicyRules policyRules{reopened};
    check(policy.mode() == "full" && policy.forbidden("release", Action::repository_read, "github.com/example/private"),
          "Reopening lost settings or Never rules");
    policyRules.setMode(localOwner, "supervised");
    FixtureModel model;
    model.action = {"request_bot_change", {{"operation", "create_bot"}, {"targetBotId", "new-bot"}, {"name", "New Bot"}}, "Create"};
    Adapter adapter;
    int applied = 0;
    RuntimeOptions options;
    options.applyBotChange = [&](const BotChange&) { ++applied; };
    Runtime runtime(reopened, model, adapter, [](auto) { return std::string{}; }, options);
    runtime.runConversations();
    const auto items = Inbox(reopened).inbox(requester);
    check(items.size() == 1 && items.at(0).at("conversationEventId") == oldMessage, "Reopening lost the existing turn or its approval");
    Inbox(reopened).decideInbox(requester, target(items.at(0)), "allow_once");
    runtime.runConversations();
    check(applied == 1, "Reopened turn could not consume approval");
}

void reopenDirectTask(const std::filesystem::path& path) {
    Id task;
    {
        Store store(path);
        BotAccess{store}.setOwner(requester, "release");
        PolicyRules{store}.setMode(localOwner, "supervised");
        Memory(store).memory(requester, "release", "apply_memory_edit", {{"baseVersion", 0}, {"content", "Use the old signing process."}});
        task = TaskIntake(store).submit({"release", "Build the release", "env", "project", Json::object()}, &requester);
    }
    Store reopened(path);
    FixtureModel model;
    Adapter adapter;
    Runtime runtime(reopened, model, adapter, [](auto) { return std::string{}; });
    for (int i = 0; i < 6; ++i) runtime.tick();
    const auto items = Inbox(reopened).inbox(requester);
    check(items.size() == 1 && items.at(0).at("taskId") == task && items.at(0).at("expiresAt") == 0 && adapter.sends == 0,
          "A saved direct request lost its approval for the original requester");
    check(items.dump().find("Use the old signing process.") == std::string::npos, "The approval displayed the Bot's private memory");
    Memory(reopened).memory(requester, "release", "apply_memory_edit",
                            {{"baseVersion", 1}, {"content", "Use the current signing process."}});
    Inbox(reopened).decideInbox(requester, target(items.at(0)), "allow_once");
    for (int i = 0; i < 6; ++i) runtime.tick();
    check(adapter.sends == 1, "The saved direct request did not launch after approval");
    check(adapter.instruction.find("Use the current signing process.") != std::string::npos &&
              adapter.instruction.find("Use the old signing process.") == std::string::npos,
          "The approved work did not use memory current at dispatch");
}

void turns(const std::filesystem::path& path) {
    Store store(path);
    PolicyRules policyRules{store};
    BotAccess botAccess{store};
    botAccess.setOwner(requester, "release");
    botAccess.setOwner(requester, "target");
    botAccess.setMember(requester, "release", member.id, true);
    policyRules.setMode(localOwner, "supervised");
    Seconds now = 1'800'000'000;
    FixtureModel model;
    Adapter adapter;
    int actions = 0;
    RuntimeOptions options;
    options.clock = [&] { return now; };
    options.applyBotChange = [&](const BotChange&) { ++actions; };
    Runtime runtime(store, model, adapter, [](auto) { return std::string{}; }, options);
    int sequence = 0;
    auto send = [&](std::string_view text, const Person& actor = requester) {
        const auto receipt = ConversationIntake(store).receive(actor, intake, std::to_string(++sequence), text);
        runtime.runConversations();
        return receipt.at("messageId").get<Id>();
    };
    auto rename = [&] {
        model.action = {
            "request_bot_change", {{"operation", "rename_bot"}, {"targetBotId", "target"}, {"name", "Release candidate"}}, "Rename"};
    };
    auto card = [&] {
        const auto items = Inbox(store).inbox(requester);
        check(items.size() == 1, "expected one card at message " + std::to_string(sequence) + ": " + items.dump());
        return items.at(0);
    };
    rename();
    const auto message = send("Rename the Bot");
    auto item = card();
    check(item.at("conversationEventId") == message && item.at("expiresAt") == 0 &&
              item.at("prompt").dump().find("Release candidate") != std::string::npos && actions == 0,
          "approval was detached, lacked its preview, or ran early");
    check(item.at("canDecide") == true && item.at("canSaveRule") == false,
          "the requester needs decision controls without global rule access");
    const auto otherView = Inbox(store).inbox(member).at(0);
    check(otherView.at("canDecide") == false && otherView.at("canSaveRule") == false,
          "member received another person's decision or rule controls");
    rejected<DecisionDenied>([&] { Inbox(store).decideInbox(member, target(item), "always_allow"); });
    rejected<DecisionDenied>([&] { Inbox(store).decideInbox(requester, target(item), "always_allow"); });
    check(card() == item && policyRules.rules().empty() && Inbox(store).decisionHistory(requester).empty(),
          "a rejected global rule affected the pending approval");
    rejected([&] { Inbox(store).decideInbox(Person{}, target(item), "allow_once"); });
    now += 3601;
    check(card().at("status") == "pending", "a person's approval expired");
    auto answer = [&] {
        try {
            Inbox(store).decideInbox(requester, target(item), "allow_once");
            return 1;
        } catch (const Conflict&) {
            return 0;
        }
    };
    auto first = std::async(std::launch::async, answer);
    auto second = std::async(std::launch::async, answer);
    check(first.get() + second.get() == 1, "two answers won the same card");
    runtime.runConversations();
    runtime.runConversations();
    check(actions == 1 && policyRules.rules().empty(), "Allow once was replayed or saved a rule");

    model.retry = true;
    send("Rename again");
    item = card();
    Inbox(store).decideInbox(requester, target(item), "deny");
    runtime.runConversations();
    check(actions == 1 && model.outcome.size() == 2 && Inbox(store).inbox(requester).empty(),
          "Retrying a denied operation created another approval or ran the operation");
    model.retry = false;

    constexpr int rulesPerKind = 32;
    for (auto kind : {RuleKind::allow, RuleKind::ask})
        for (int i = 0; i < rulesPerKind; ++i)
            policyRules.changeRule(localOwner, kind, Action::bot_rename, "fixture-" + std::to_string(i), true);
    const auto existingRules = policyRules.rules();
    check(existingRules.size() == 2 * rulesPerKind, "Adding many rules did not keep every Allow and Ask-first rule");
    send("New request after the denial");
    item = card();
    const Person delegatedAdmin{"delegated-admin", true};
    bool isAdmin = true;
    policyRules.requireWebIdentity();
    WebIdentity(store).setWebIdentityResolver(
        [&](std::string_view person) -> std::optional<bool> {
            if (person == delegatedAdmin.id) return isAdmin;
            if (person == localOwner.id) return true;
            if (person == requester.id) return false;
            return std::nullopt;
        },
        true);
    check(Inbox(store).inbox(delegatedAdmin).at(0).at("canSaveRule") == true, "an administrator could not save a global rule");
    isAdmin = false;
    check(Inbox(store).inbox(delegatedAdmin).empty(), "a demoted administrator retained access to the approval");
    rejected<DecisionDenied>([&] { Inbox(store).decideInbox(delegatedAdmin, target(item), "always_allow"); });
    check(card() == item && policyRules.rules() == existingRules, "a stale administrator changed the rule or card");
    // Older host configuration can assign separate credentials with distinct
    // roles to one person. Interactive authority comes from the credential.
    WebIdentity(store).setWebIdentityResolver([](std::string_view) -> std::optional<bool> { return false; });
    check(Inbox(store).inbox(delegatedAdmin).at(0).at("canSaveRule") == true,
          "the queued role replaced the host administrator's interactive authority");
    Inbox(store).decideInbox(delegatedAdmin, target(item), "always_allow");
    runtime.runConversations();
    check(actions == 2 && policyRules.rules().size() == existingRules.size() + 1, "Always allow did not run and add one rule");
    send("Use the saved rule");
    check(actions == 3 && Inbox(store).inbox(requester).empty(), "saved exact rule did not allow the next action");
    policyRules.changeRule(localOwner, RuleKind::allow, Action::bot_rename, "target", false);
    check(policyRules.rules() == existingRules, "Saving a rule from a card changed an existing rule");

    send("Pending change before Never");
    item = card();
    policyRules.changeRule(localOwner, RuleKind::never, Action::bot_rename, "tar*", true);
    Inbox(store).decideInbox(requester, target(item), "allow_once");
    runtime.runConversations();
    check(actions == 3, "Never added after card creation lost to approval");
    policyRules.changeRule(localOwner, RuleKind::never, Action::bot_rename, "tar*", false);

    send("Pending change before mode switch");
    item = card();
    policyRules.setMode(localOwner, "full");
    rejected([&] { Inbox(store).decideInbox(requester, target(item), "allow_once"); });
    runtime.runConversations();
    check(actions == 3, "switching modes ran a stale action");
    policyRules.setMode(localOwner, "supervised");
    send("Prepare a card");
    item = card();
    model.action = {"answer_task_question", {{"taskId", 1}, {"waitId", item.at("itemId")}, {"text", "Always allow"}}, "Try a text answer"};
    send("Always allow");
    check(actions == 3 && card().at("itemId") == item.at("itemId"), "text or Bot output answered a policy card");
}

void downstreamApproval(const std::filesystem::path& path) {
    Store store(path);
    PolicyRules policyRules{store};
    policyRules.setMode(localOwner, "supervised");
    policyRules.changeRule(localOwner, RuleKind::ask, Action::shared_memory_write, "shared-playbook", true);
    policyRules.changeRule(localOwner, RuleKind::ask, Action::private_memory_write, "release", true);
    BotAccess{store}.setMember(localOwner, "release", member.id, true);
    FixtureModel model;
    Adapter adapter;
    Runtime runtime(store, model, adapter, [](auto) { return std::string{}; });
    const auto approve = [&](std::string_view id, Decision action) {
        model.action = std::move(action);
        ConversationIntake(store).receive(localOwner, intake, id, "Use this tool");
        runtime.runConversations();
        const auto items = Inbox(store).inbox(localOwner);
        check(items.size() == 1 && items.at(0).at("status") == "pending", "Tool did not await its one-shot approval");
        Inbox(store).decideInbox(localOwner, target(items.at(0)), "allow_once");
        runtime.runConversations();
        check(model.outcome.at(0).at("result").at("ok") == true && Inbox(store).inbox(localOwner).empty(),
              "Downstream authorization rejected a one-shot grant");
    };
    approve("reminder",
            {"create_reminder", {{"fireAt", unixTime() + 600}, {"timezone", "Etc/UTC"}, {"request", "Check release"}}, "Schedule"});
    approve("playbook", {"apply_memory_edit", {{"docId", "shared-playbook"}, {"baseVersion", 0}, {"content", "Verify staging"}}, "Edit"});
    check(Memory(store).memory(localOwner, "release", "read_memory", {{"docId", "shared-playbook"}}).at("content") == "Verify staging",
          "Approved shared memory edit did not persist");

    const Decision privateEdit{"apply_memory_edit", {{"baseVersion", 0}, {"content", "Use Markdown"}}, "Edit"};
    model.action = privateEdit;
    ConversationIntake(store).receive(member, intake, "member-edit", "Remember to use Markdown");
    runtime.runConversations();
    check(Inbox(store).inbox(localOwner).empty(), "An ineligible private memory edit requested approval");
    check(model.outcome.at(0).at("result") == Json{{"ok", false}, {"error", "Memory access denied"}},
          "The shared member's private memory edit was not denied");
    approve("private-edit", privateEdit);
    check(Memory(store).memory(localOwner, "release", "read_memory", Json::object()).at("content") == "Use Markdown",
          "Approved private memory edit did not persist");
}

void scheduled(const std::filesystem::path& path) {
    Store store(path);
    PolicyRules policyRules{store};
    BotAccess{store}.setOwner(requester, "release");
    policyRules.setMode(localOwner, "supervised");
    Seconds now = 1'800'000'000;
    FixtureModel model;
    Adapter adapter;
    RuntimeOptions options;
    options.clock = [&] { return now; };
    Runtime runtime(store, model, adapter, [](auto) { return std::string{}; }, options);
    const auto pump = [&] {
        for (int i = 0; i < 6; ++i) runtime.tick();
    };
    const auto anonymous = TaskIntake(store).submit({"release", "No person originated this task", "env", "anonymous-project"});
    pump();
    check(adapter.sends == 0 &&
              std::ranges::none_of(Inbox(store).inbox(requester),
                                   [](const auto& item) { return item.at("itemKind") == "policy_approval"; }) &&
              model.outcome.dump().find("denied by policy") != std::string::npos,
          "unanswerable task received a card or was silently denied");
    TaskControl(store).operate(requester, TaskTarget::from(Tasks(store).task(anonymous).task, 0), "cancel");
    pump();

    const auto schedule = [&] {
        Automations(store).createAutomation(
            requester, intake,
            {{"kind", "reminder"}, {"timezone", "Etc/UTC"}, {"fireAt", now + 1}, {"request", "Build the scheduled release"}}, now);
        ++now;
        pump();
    };
    const auto staleTask = TaskIntake(store).submit({"release", "Cancel a pending launch", "env", "stale-project"}, &requester);
    pump();
    const auto staleItem = Inbox(store).inbox(requester).at(0);
    check(staleItem.at("taskId") == staleTask && staleItem.at("prompt").dump().find("Build the release") != std::string::npos,
          "pending launch card did not bind the task and its prepared instruction");
    auto stale = target(staleItem);
    check(taskJson(Tasks(store).task(staleTask)).at("wait").at("reason") == "policy_approval", "task never entered its durable wait");
    Inbox(store).decideInbox(requester, stale, "allow_once");
    TaskControl(store).operate(requester, TaskTarget::from(Tasks(store).task(staleTask).task, 0), "cancel");
    pump();
    check(adapter.sends == 0, "an approved action ran after the task was cancelled");
    rejected([&] { Inbox(store).decideInbox(requester, stale, "allow_once"); });

    schedule();
    auto items = Inbox(store).inbox(requester);
    check(items.size() == 1 && items.at(0).at("expiresAt") == now + 1800, "background approval has the wrong recipient or expiry");
    const auto item = items.at(0);
    now += 1799;
    check(Inbox(store).inbox(requester).at(0).at("status") == "pending", "approval expired early");
    ++now;
    rejected([&] { Inbox(store).decideInbox(requester, target(item), "allow_once"); });
    const auto expired = Inbox(store).inbox(requester).at(0);
    check(expired.at("status") == "expired" && expired.at("canDecide") == false && expired.at("canSaveRule") == false,
          "the expired approval showed actions to the requester");
    rejected<DecisionDenied>([&] { Inbox(store).decideInbox(requester, target(expired), "allow_next_time"); });
    check(Inbox(store).inbox(requester).at(0) == expired && policyRules.rules().empty(),
          "the requester changed an expired approval or its global rule");
    const auto adminView = Inbox(store).inbox(localOwner).at(0);
    check(adminView.at("canDecide") == true && adminView.at("canSaveRule") == true,
          "the administrator could not act on the expired approval");
    Inbox(store).decideInbox(localOwner, target(expired), "allow_next_time");
    pump();
    check(adapter.sends == 0 && model.outcome.dump().find("expired") != std::string::npos, "expired action ran or the Bot was never told");
    TaskControl(store).operate(requester, TaskTarget::from(Tasks(store).task(item.at("taskId")).task, 0), "cancel");
    pump();
    schedule();
    check(adapter.sends == 1 && Inbox(store).inbox(requester).empty(), "next-time rule did not admit only future work");
}

void channel(const std::filesystem::path& path) {
    Store store(path);
    PolicyRules policyRules{store};
    BotAccess botAccess{store};
    botAccess.setOwner(requester, "release");
    botAccess.setMember(requester, "release", member.id, true);
    policyRules.setMode(localOwner, "supervised");
    WebIdentity(store).setWebIdentityResolver([](std::string_view id) -> std::optional<bool> {
        if (id == requester.id) return false;
        return std::nullopt;
    });
    struct Transport : ChannelTransport {
        unsigned reactions = 0, artifacts = 0;
        std::vector<std::string> replies;
        ChannelDelivery send(std::string_view, const ChannelOrigin&, std::string_view tool, const Json& payload) override {
            if (tool == "channel_react") ++reactions;
            if (tool == "publish_artifact") ++artifacts;
            if (tool == "reply") replies.push_back(payload.at("text"));
            return {ChannelDelivery::State::accepted, "posted"};
        }
        ChannelDelivery reconcile(std::string_view, const ChannelOrigin&) override { return {}; }
    };
    auto transport = std::make_shared<Transport>();
    Channels(store).artifactBaseUrl("https://previews.example.com");
    Channels(store).connectChannel({ChannelKind::slack, channelCapabilities(ChannelKind::slack), [](auto, auto) { return true; },
                                    [](auto, auto sender) -> std::optional<ChannelSender> {
                                        return ChannelSender{"release", sender == "linked" ? requester.id : member.id};
                                    },
                                    transport});
    FixtureModel model;
    Adapter adapter;
    Runtime runtime(store, model, adapter, [](auto) { return std::string{}; });
    const Decision publish{"publish_artifact", {{"title", "Status"}, {"html", "<p>Status</p>"}}, "Publish"};
    model.action = publish;
    auto receive = [&](std::string_view id, std::string_view text, std::string_view sender = "external") {
        Channels(store).receiveChannel(
            ChannelKind::slack, "fixture",
            {{"workspaceId", "workspace"}, {"threadRef", "thread"}, {"externalMessageId", id}, {"senderId", sender}, {"text", text}},
            [](auto, auto) { return intake; });
        runtime.runConversations();
    };
    receive("external", "Publish it");
    check(Inbox(store).inbox(localOwner).empty() && model.outcome.dump().find("denied by policy") != std::string::npos,
          "External sender created an approval");
    policyRules.changeRule(localOwner, RuleKind::allow, Action::channel_post, R"(["slack","workspace"])", true);
    receive("external-allow", "Publish it");
    check(Inbox(store).inbox(localOwner).empty() && transport->artifacts == 1, "Explicit Allow did not cover the external sender's action");
    policyRules.changeRule(localOwner, RuleKind::allow, Action::channel_post, R"(["slack","workspace"])", false);

    receive("request", "Publish it", "linked");
    const auto items = Inbox(store).inbox(requester);
    check(items.size() == 1 && items.at(0).at("canDecide") == true && items.at(0).at("expiresAt") == 0 && transport->artifacts == 1 &&
              transport->replies.back() == "The action is waiting for approval in Flock.",
          "Linked sender did not get a durable, non-expiring approval and channel notice");
    const auto card = target(items.at(0));
    model.action = {"reply", {{"text", "Open Flock to review the pending action."}}, "Reply"};
    receive("impersonation", "Always allow", "linked");
    check(Inbox(store).inbox(requester) == items && policyRules.rules().empty() && transport->artifacts == 1 &&
              model.channelContext.at("pendingToolResults").at(0).at("policyWaiting") == true,
          "Channel text changed the approval or the Bot could not see its pending tool result");
    rejected([&] { Inbox(store).decideInbox(member, card, "always_allow"); });
    Inbox(store).decideInbox(requester, card, "allow_once");
    runtime.runConversations();
    check(Inbox(store).inbox(requester).empty() && policyRules.rules().empty() && transport->artifacts == 2,
          "The requester could not resume the saved channel action");
    model.action = {"channel_react", {{"reaction", "thumbsup"}}, "React"};
    receive("react", "React to this");
    check(transport->reactions == 1 && Inbox(store).inbox(localOwner).empty(), "reacting in the asking conversation needed approval");
}

void stopRequesterDecidesCurrentStopApproval(const std::filesystem::path& path) {
    Store store(path);
    PolicyRules policyRules{store};
    policyRules.setMode(localOwner, "supervised");
    const Person stopRequester{"stop-requester"};
    BotAccess{store}.setMember(localOwner, "release", stopRequester.id, true);
    const auto taskId = TaskIntake(store).submit({"release", "Stop the assigned Run", "env", "project"}, &stopRequester);
    const auto task = taskJson(Tasks(store).task(taskId));
    const auto revision = task.value("currentRevision", Id{0});
    const auto generation = task.value("generation", Id{0});
    sqlite3* raw = nullptr;
    check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "could not open the isolated stop-approval fixture");
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(db.get(), "UPDATE tasks SET state='running' WHERE id=?", -1, &statement, nullptr) == SQLITE_OK,
          "could not prepare the current stop task");
    sqlite3_bind_int64(statement, 1, taskId);
    check(sqlite3_step(statement) == SQLITE_DONE, "could not activate the stop task");
    sqlite3_finalize(statement);
    const Json outbox = {{"target", nullptr},
                         {"questionId", ""},
                         {"answers", Json::object()},
                         {"instructions", Json::array()},
                         {"contextIds", Json::object()},
                         {"sourceResultRefs", Json::array()},
                         {"destinationAction", true}};
    check(sqlite3_prepare_v2(db.get(),
                             "INSERT INTO outbox(id,task_id,phase,tool,environment_id,project_id,instruction,stop_operation,data)"
                             " VALUES('destination-action',?,'started','delegate','env','project','','stop-request-1',?)",
                             -1, &statement, nullptr) == SQLITE_OK,
          "could not prepare the started destination outbox");
    sqlite3_bind_int64(statement, 1, taskId);
    const auto outboxText = outbox.dump();
    sqlite3_bind_text(statement, 2, outboxText.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement) == SQLITE_DONE, "could not persist the started destination outbox");
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(db.get(), "SELECT revision FROM policy_settings WHERE id=1", -1, &statement, nullptr) == SQLITE_OK &&
              sqlite3_step(statement) == SQLITE_ROW,
          "could not read the current policy revision");
    const auto modeRevision = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(db.get(), R"SQL(
INSERT INTO policy_approvals(operation,bot,action,target,fingerprint,preview,scope,requester,anchor,turn,task,
 revision,generation,created_at,expires_at,mode_revision,status,wait_id,card_generation,resume_state)
VALUES('run-stop/stop-request-1','release','t3_send','cloud:thread','fixture','Stop the assigned Run',?,?,?,?,?,?,?, ?,0,?,'pending',0,3,'')
)SQL",
                             -1, &statement, nullptr) == SQLITE_OK,
          "could not prepare the stop-requester approval");
    const auto scope = "task/" + std::to_string(taskId);
    sqlite3_bind_text(statement, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, stopRequester.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, 0);
    sqlite3_bind_int64(statement, 4, 0);
    sqlite3_bind_int64(statement, 5, taskId);
    sqlite3_bind_int64(statement, 6, revision);
    sqlite3_bind_int64(statement, 7, generation);
    sqlite3_bind_int64(statement, 8, unixTime());
    sqlite3_bind_int64(statement, 9, modeRevision);
    check(sqlite3_step(statement) == SQLITE_DONE, "could not persist the stop-requester approval");
    sqlite3_finalize(statement);

    const auto cards = Inbox(store).inbox(stopRequester, "release");
    check(cards.size() == 1 && cards.at(0).at("canDecide") == true && cards.at(0).at("canSaveRule") == false &&
              cards.at(0).at("taskId") == taskId,
          "the stop requester did not receive the current one-shot approval");
    Inbox(store).decideInbox(stopRequester, target(cards.at(0)), "allow_once");
    check(sqlite3_prepare_v2(db.get(), "SELECT status FROM policy_approvals WHERE operation='run-stop/stop-request-1'", -1, &statement,
                             nullptr) == SQLITE_OK &&
              sqlite3_step(statement) == SQLITE_ROW &&
              std::string_view(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))) == "approved",
          "Store::decideInbox did not accept the authorized stop requester");
    sqlite3_finalize(statement);
}

void secrets(const std::filesystem::path& path) {
    Store store(path);
    SecretRequests secretRequests{store};
    BotAccess{store}.setOwner(requester, "release");
    PolicyRules{store}.setMode(localOwner, "supervised");
    const SecretRef secret{"deployment-token"};
    const auto request = secretRequests.requestSecret("release", secret);
    constexpr auto value = "SECRET_FIXTURE_VALUE";
    secretRequests.submitSecret(requester, "release", request, value);
    auto task = TaskIntake(store).submit({"release", "Use the token", "env", "project"}, &requester);
    unsigned uses = 0;
    const auto adapter = [&](std::string_view token) {
        check(token == value, "protected value did not reach the adapter");
        ++uses;
    };
    rejected([&] { secretRequests.useSecret(task, secret, "connector.example", "first", true, adapter); });
    auto items = Inbox(store).inbox(requester);
    check(items.size() == 1 && items.dump().find(value) == std::string::npos && uses == 0, "protected value reached the card");
    Inbox(store).decideInbox(requester, target(items.at(0)), "allow_once");
    secretRequests.useSecret(task, secret, "connector.example", "first", true, adapter);
    rejected([&] { secretRequests.useSecret(task, secret, "connector.example", "first", true, adapter); });
    secretRequests.useSecret(task, secret, "connector.example", "next", true, adapter);
    rejected([&] { secretRequests.useSecret(task, secret, "other.example", "third", true, adapter); });
    check(uses == 2 && Inbox(store).inbox(requester).size() == 1, "secret destination did not gate subsequent use");
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("flock-supervised-tests-" + actionId());
    std::filesystem::create_directory(root);
    int status = 0;
    try {
        rules(root / "rules.db");
        reopen(root / "reopen.db");
        reopenDirectTask(root / "direct-request.db");
        turns(root / "turns.db");
        downstreamApproval(root / "one-shot.db");
        scheduled(root / "scheduled.db");
        stopRequesterDecidesCurrentStopApproval(root / "stop-requester.db");
        secrets(root / "secrets.db");
        channel(root / "channel.db");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    std::filesystem::remove_all(root);
    return status;
}
