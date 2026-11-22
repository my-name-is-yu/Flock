#include "internal.hpp"
#include "flock/session_settings.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>

namespace flockv0 {
namespace t3detail {
namespace {
using Outcome = Delivery::Kind;
using Observed = Observation::Kind;
using Position = Id;
using Clock = std::chrono::steady_clock;
constexpr int ExcerptChars = 2730; // T3 uses UTF-16 offsets. 2730 code units occupy at most 8190 UTF-8 bytes.
constexpr int PageSize = 30;
constexpr auto ThreadRead = "t3_thread_read";
constexpr auto PendingRequest = "t3_pending_request";

struct Scan {
    Clock::time_point expires = Clock::now() + std::chrono::seconds(15);
    int remaining = 160;
    void step() {
        if (--remaining < 0 || Clock::now() >= expires)
            throw Failure(Failure::Kind::Wire, "The bounded T3 history search could not finish");
    }
};

Delivery result(Outcome kind, std::string message = {}, RunRef run = {}) {
    return {kind, std::move(run), std::move(message)};
}

Observation state(Observed kind, std::string status, bool settled = false) {
    Observation observation;
    observation.kind = kind;
    observation.status = std::move(status);
    observation.settled = settled;
    return observation;
}

bool terminal(std::string_view status) {
    return status == "completed" || status == "failed" || status == "cancelled" ||
           status == "interrupted" || status == "rolled_back";
}

bool active(std::string_view status) {
    return status == "preparing" || status == "queued" || status == "starting" ||
           status == "running" || status == "waiting";
}

bool hasRun(const RunRef& run) { return !run.threadId.empty() && !run.runId.empty(); }

std::optional<Position> cursor(const Json& response, const char* key) {
    const auto& value = member(response, key);
    return value.is_null() ? std::nullopt : std::optional(position(value));
}

Position readCursor(const std::string& text) {
    if (text.empty()) return -1;
    Position parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc() || end != text.data() + text.size() || parsed < 0 || parsed > maxHistoryPosition)
        throw Failure(Failure::Kind::Settings, "Invalid T3 history cursor");
    return parsed;
}

const Json& detail(const Json& response, const std::string& threadId) {
    const auto& thread = member(response, "thread");
    if (str(thread, "threadId") != threadId) invalidResponse();
    return thread;
}

Json threadHead(Client& client, const std::string& threadId, int runLimit = 1) {
    return readThread(client,threadId,"messages",1,32,Json::object(),runLimit);
}

bool fromRun(const Json& item, const RunRef& run) {
    return localItem(item, run.threadId) && str(item, "runId") == run.runId;
}

template<typename Visit>
bool walkThreadHistoryForward(Client& client, Scan& scan, const std::string& threadId, std::string_view view,
          int maxChars, Position after, Visit visit) {
    for (;;) {
        scan.step();
        const auto start = after;
        const auto page = readThread(client,threadId,view,PageSize,maxChars,{{"afterPosition",after}});
        for (const auto& item : array(page,"items")) {
            advancePosition(item,after);
            if (!visit(item)) return false;
        }
        if (!moreHistory(page,after,start)) return true;
    }
}

struct TextPage { std::string text; std::size_t next = 0; bool more = false; };
TextPage itemText(const Json& item, std::size_t offset = 0) {
    const auto& field = member(item, "text");
    if (!field.is_null() && !field.is_string()) invalidResponse();
    TextPage result{str(item, "text"), 0, boolean(item, "textTruncated")};
    if (result.more) {
        const auto next = cursor(item, "nextTextOffset");
        if (!next || static_cast<std::size_t>(*next) <= offset || result.text.empty()) invalidResponse();
        result.next = static_cast<std::size_t>(*next);
    }
    return result;
}

Json readItem(Client& client, const std::string& threadId, std::string_view view,
              const std::string& itemId, std::size_t offset, int chars = ExcerptChars) {
    if (itemId.empty() || offset > maxHistoryPosition) invalidResponse();
    const auto response = readThread(client,threadId,view,1,chars,{{"itemId",itemId},{"textOffset",offset}});
    const auto& items = array(response, "items");
    if (items.size() != 1 || str(items.front(), "itemId") != itemId) invalidResponse();
    return items.front();
}

std::optional<std::string> readCompleteItemText(Client& client, Scan& scan, const Json& item,
        const std::string& threadId, std::string_view view, std::size_t maxBytes) {
    auto page = itemText(item);
    auto output = std::move(page.text);
    while (page.more && output.size() <= maxBytes) {
        scan.step();
        const auto next = readItem(client, threadId, view, str(item, "itemId"), page.next);
        if (str(next, "sourceThreadId") != str(item, "sourceThreadId") ||
            str(next, "runId") != str(item, "runId") || str(next, "type") != str(item, "type") ||
            str(next, "updatedAt") != str(item, "updatedAt")) return std::nullopt;
        page = itemText(next, page.next);
        output += page.text;
    }
    if (page.more || output.size() > maxBytes) return std::nullopt;
    return output;
}

Json wait(Client& client, const RunRef& run, bool delegated, int timeout = 400) {
    const auto response = client.call("t3_thread_wait", {{"threadId", run.threadId}, {"runId", run.runId},
        {"timeoutMs", timeout}, {"untilDelegatedWorkSettled", delegated}});
    if (str(response, "threadId") != run.threadId || str(response, "runId").empty()) invalidResponse();
    boolean(response, "timedOut");
    return response;
}

bool sameDelegateTitle(std::string_view activity, std::string_view notice) {
    if (activity == notice) return true;
    // T3 truncates the activity title and the completion notice independently.
    const auto stem = [](std::string_view title) {
        if (title.ends_with("...")) return title.substr(0,title.size()-3);
        if (title.ends_with("…")) return title.substr(0,title.size()-std::string_view("…").size());
        return title;
    };
    const auto first = stem(activity);
    const auto second = stem(notice);
    return (first.size() >= 48 && first.size() != activity.size() && second.starts_with(first)) ||
           (second.size() >= 48 && second.size() != notice.size() && first.starts_with(second));
}

struct CompletionNotice { std::string title, updatedAt; };
struct DelegatedChild {
    std::string parentRun, title, settledAt;
    bool consumed = false;
};

std::optional<CompletionNotice> completionNotice(const Json& item) {
    constexpr std::string_view header = "Delegated task \"";
    constexpr std::string_view footer = "\" finished";
    const auto text = str(item,"text");
    const auto updatedAt = str(item,"updatedAt");
    if (str(item,"status") != "completed" || boolean(item,"textTruncated") || updatedAt.empty() ||
        !text.starts_with(header) || !text.ends_with(footer) || text.size() <= header.size() + footer.size())
        return std::nullopt;
    return CompletionNotice{text.substr(header.size(),text.size() - header.size() - footer.size()),updatedAt};
}

bool consumeCompletion(const CompletionNotice& notice, std::map<std::string,DelegatedChild>& children,
                       const std::set<std::string>& ownedRuns) {
    DelegatedChild* match = nullptr;
    for (auto& [id,child] : children) {
        if (!sameDelegateTitle(child.title,notice.title)) continue;
        if (match) return false;
        match = &child;
    }
    if (!match || match->consumed || !ownedRuns.contains(match->parentRun) ||
        match->settledAt.empty() || match->settledAt > notice.updatedAt) return false;
    match->consumed = true;
    return true;
}

bool ownsResult(Client& client, const RunRef& requested, const std::string& effectiveRunId,
                const std::string& resultRunId) {
    if (resultRunId == effectiveRunId || resultRunId == requested.runId) return true;
    const auto head = threadHead(client, requested.threadId, 50);
    std::map<Position,std::string> ordered;
    Position base = 0, ending = 0;
    for (const auto& run : array(head,"recentRuns")) {
        const auto id = str(run,"runId");
        const auto ordinal = position(member(run,"ordinal"));
        if (id.empty() || !ordered.emplace(ordinal,id).second) invalidResponse();
        if (id == effectiveRunId) base = ordinal;
        if (id == resultRunId) ending = ordinal;
    }
    if (base == 0 || ending <= base) return false;
    std::map<std::string,std::vector<CompletionNotice>> notices;
    for (auto ordinal = base + 1; ordinal <= ending; ++ordinal) {
        const auto found = ordered.find(ordinal);
        if (found == ordered.end()) return false;
        notices.try_emplace(found->second);
    }

    std::map<std::string,DelegatedChild> children;
    Scan scan;
    const bool complete = walkThreadHistoryForward(client,scan,requested.threadId,"activity",700,-1,[&](const Json& item) {
        if (!localItem(item,requested.threadId)) return true;
        const auto run = str(item,"runId");
        const auto type = str(item,"type");
        if (type == "subagent") {
            const auto id = str(item,"subagentId");
            const auto title = str(item,"title");
            if (!id.empty() && !title.empty())
                children.emplace(id,DelegatedChild{run,title,terminal(str(item,"status")) ? str(item,"updatedAt") : ""});
        }
        const auto candidate = notices.find(run);
        if (candidate == notices.end()) return true;
        if (type == "user_message") return false;
        if (type == "notification") {
            auto notice = completionNotice(item);
            if (!notice) return false;
            candidate->second.push_back(std::move(*notice));
        }
        return true;
    });
    if (!complete) return false;

    std::set<std::string> ownedRuns{requested.runId,effectiveRunId};
    for (auto ordinal = base + 1; ordinal <= ending; ++ordinal) {
        const auto& run = ordered.at(ordinal);
        const auto& inputs = notices.at(run);
        if (inputs.empty()) return false;
        for (const auto& notice : inputs)
            if (!consumeCompletion(notice,children,ownedRuns)) return false;
        ownedRuns.insert(run);
    }
    return true;
}

std::set<std::string> pending(Client& client, const std::string& threadId) {
    const auto data = client.call(PendingRequest, {{"action", "list"}, {"threadId", threadId}});
    std::set<std::string> requests;
    for (const auto& request : array(data, "requestIds")) {
        if (!request.is_string() || request.get_ref<const std::string&>().empty() || !requests.insert(request).second)
            invalidResponse();
    }
    return requests;
}

Json readQuestion(Client& client, const std::string& threadId, const std::string& requestId) {
    const auto result = client.call(PendingRequest, {{"action", "read"}, {"threadId", threadId}, {"requestId", requestId}});
    if (str(result, "requestId") != requestId) invalidResponse();
    return array(result, "questions");
}

Json questionFields(const Json& questions) {
    if (!questions.is_array() || questions.empty()) return {};
    std::set<std::string> ids;
    auto fields = Json::array();
    for (const auto& question : questions) {
        const auto id = str(question, "id");
        if (id.empty() || !ids.insert(id).second || trimSessionQuestionText(str(question, "question")).empty() ||
            !question.contains("options") || !question["options"].is_array()) return {};
        for (const auto key : {"allowCustomAnswer", "multiSelect", "required"}) {
            if (question.contains(key) && !question[key].is_boolean()) return {};
        }
        const bool freeform = question.value("allowCustomAnswer", true);
        const bool required = question.value("required", true);
        auto options = Json::array();
        std::set<std::string> values;
        for (const auto& option : question["options"]) {
            const auto label = str(option, "label");
            const auto specified = option.find("value");
            const auto trimmedLabel = trimSessionQuestionText(label);
            if (trimmedLabel.empty() || (specified != option.end() && !specified->is_string())) return {};
            const auto value = specified == option.end() ? std::string(trimmedLabel) : specified->get<std::string>();
            if (!values.insert(value).second) return {};
            options.push_back({{"label", label}, {"value", value}, {"description", str(option, "description")}});
        }
        if (!freeform && required && options.empty()) return {};
        Json field = {{"id", id}, {"question", str(question, "question")}, {"header", str(question, "header")},
                      {"required", required}, {"multiple", question.value("multiSelect", false)},
                      {"freeform", freeform}, {"options", std::move(options)}};
        if (question.contains("initialAnswer") && question["initialAnswer"].is_string())
            field["initialAnswer"] = question["initialAnswer"];
        fields.push_back(std::move(field));
    }
    return fields;
}

struct Attribution {
    enum class Kind { Owned, Ambiguous, Resolved } kind;
    Json fields;
    Json raw;
};

Attribution attribute(Client& client, const RunRef& run, const std::string& requestId) {
    Json questions;
    try { questions = readQuestion(client, run.threadId, requestId); }
    catch (const Failure& error) {
        if (error.reason == Failure::Reason::QuestionGone) return {Attribution::Kind::Resolved, {}, {}};
        throw;
    }
    auto fields = questionFields(questions);
    if (fields.is_null()) return {Attribution::Kind::Ambiguous, {}, {}};
    int matching = 0;
    bool exactOwner = false, unverified = false;
    Scan scan;
    try {
        walkThreadHistoryForward(client, scan, run.threadId, "activity", 800, -1, [&](const Json& item) {
            if (str(item, "type") != "user_input_request") return true;
            const auto text = readCompleteItemText(client, scan, item, run.threadId, "activity", 65536);
            if (!text) { unverified = true; return true; }
            const auto activityQuestions = Json::parse(*text, nullptr, false);
            if (!activityQuestions.is_array()) { unverified = true; return true; }
            if (activityQuestions == questions) {
                ++matching;
                exactOwner = fromRun(item, run) && str(item, "status") == "waiting";
            }
            return matching < 2;
        });
    } catch (const Failure& failure) {
        if (failure.kind == Failure::Kind::Auth || failure.kind == Failure::Kind::Settings) throw;
        return {Attribution::Kind::Ambiguous, {}, {}};
    }
    if (unverified || matching != 1 || !exactOwner) return {Attribution::Kind::Ambiguous, {}, {}};
    return {Attribution::Kind::Owned, std::move(fields), std::move(questions)};
}

void useLocalThreadLink(RunRef& run, std::string_view environment, std::string_view origin) {
    // checkedUrl permits plaintext HTTP only on loopback. A local-only T3 environment
    // opens in its own browser client, where the user authorized this connection.
    if (origin.starts_with("http://") && !run.threadId.empty())
        run.link = std::string(origin) + "/" + encode(environment) + "/" + encode(run.threadId);
}

Json launchArgs(const Command& command) {
    if (!command.config.is_object()) throw Failure(Failure::Kind::Settings, "Invalid T3 Bot settings");
    auto settings = command.config.value("session", Json::object());
    // The direct adapter API also supports interaction mode for T3 callers.
    Json interactionMode;
    if (settings.contains("interactionMode")) {
        interactionMode = std::move(settings["interactionMode"]);
        settings.erase("interactionMode");
    }
    if (!interactionMode.is_null() && interactionMode != "default" && interactionMode != "plan")
        throw Failure(Failure::Kind::Settings, "Invalid T3 interaction mode");
    Json args;
    try { args = normalizeSessionSettings(settings); }
    catch (const Conflict& error) { throw Failure(Failure::Kind::Settings, error.what()); }
    if (args.contains("model")) {
        args["modelSelection"] = {{"instanceId", std::move(args["providerInstanceId"])}, {"model", std::move(args["model"])}};
        args.erase("providerInstanceId");
        args.erase("model");
    }
    if (!interactionMode.is_null()) args["interactionMode"] = std::move(interactionMode);
    args["projectId"] = command.projectId;
    args["title"] = "Flock task";
    args["message"] = sessionPrompt(command.actionId,command.instruction);
    args["clientRequestId"] = command.actionId;
    return args;
}

void verifyCommand(const Command& command) {
    if (command.tool == "answer_session_question") {
        if (!hasRun(command.target) || command.questionId.empty() || !command.answers.is_object())
            throw Failure(Failure::Kind::Settings, "The T3 answer requires a Thread, Run, question ID and answers");
        return;
    }
    if ((command.tool != "delegate" && command.tool != "follow_up" && command.tool != "steer") ||
        (command.tool != "delegate" && !hasRun(command.target)) || command.projectId.empty() ||
        command.actionId.empty() || command.actionId.size() > 256 || trim(command.actionId) != command.actionId ||
        command.actionId.find_first_of("\r\n[]") != std::string::npos || trim(command.instruction).empty() ||
        sessionPrompt(command.actionId,command.instruction).size() > maxSessionPromptBytes)
        throw Failure(Failure::Kind::Settings, "Invalid T3 command, action ID or destination");
}

Delivery failureDelivery(const Failure& failure) {
    if (failure.kind == Failure::Kind::Auth) return result(Outcome::Auth, failure.what());
    if (failure.kind == Failure::Kind::Settings) return result(Outcome::Settings, failure.what());
    return result(Outcome::Unknown, failure.what());
}

Delivery workReply(const Failure& failure) {
    auto reply = failureDelivery(failure);
    reply.definitelyUnsent = failure.kind == Failure::Kind::Auth || failure.reason == Failure::Reason::WorkSettings ||
                             failure.reason == Failure::Reason::ProjectNotFound;
    return reply;
}

std::vector<std::string> projectThreads(Client& client, Scan& scan, const std::string& projectId) {
    std::vector<std::string> found;
    std::set<std::string> visited;
    std::optional<Position> total;
    Position offset = 0;
    for (;;) {
        scan.step();
        const auto page = client.call("t3_thread_list", {{"projectId", projectId}, {"includeSubagents", false},
                                                         {"limit", 50}, {"cursor", offset}});
        if (str(page, "projectId") != projectId) invalidResponse();
        const auto count = position(member(page, "total"));
        if (total && count != *total) invalidResponse();
        total = count;
        const auto& threads = array(page, "threads");
        for (const auto& thread : threads) {
            const auto id = str(thread, "threadId");
            if (id.empty() || str(thread, "projectId") != projectId || !visited.insert(id).second) invalidResponse();
            if (str(thread, "createdBy") == "agent" && str(thread, "creationSource") == "mcp") found.push_back(id);
        }
        const auto next = cursor(page, "nextCursor");
        const auto covered = offset + static_cast<Position>(threads.size());
        if (!next) {
            if (covered != count) invalidResponse();
            return found;
        }
        if (*next <= offset || *next != covered || *next >= count) invalidResponse();
        offset = *next;
    }
}

Delivery reconcileWork(Client& client, const Command& command) {
    const bool launch = command.tool == "delegate";
    if (!launch && !command.candidate.threadId.empty() && command.candidate.threadId != command.target.threadId)
        return result(Outcome::Unknown, "The supplied Thread is outside the saved action destination");
    Scan scan;
    auto threads = launch ? projectThreads(client, scan, command.projectId) : std::vector{command.target.threadId};
    const auto& hint = command.candidate.threadId;
    if (!hint.empty() && std::find(threads.begin(), threads.end(), hint) == threads.end()) threads.push_back(hint);
    const auto expected = sessionPrompt(command.actionId,command.instruction);
    std::map<std::pair<std::string, std::string>, RunRef> matches;
    bool incomplete = false;
    for (const auto& threadId : threads) {
        scan.step();
        const auto head = threadHead(client, threadId);
        const auto& thread = detail(head, threadId);
        if (str(thread, "projectId") != command.projectId ||
            (launch && (str(thread, "createdBy") != "agent" || str(thread, "creationSource") != "mcp"))) continue;
        walkThreadHistoryForward(client, scan, threadId, "messages", 600, -1, [&](const Json& item) {
            if (str(item, "type") != "user_message" || !localItem(item, threadId)) return true;
            const auto text = itemText(item);
            const bool possible = text.more ? expected.starts_with(text.text) : expected == text.text;
            if (possible && str(item, "createdBy") == "agent" && str(item, "creationSource") == "mcp" &&
                str(item, "senderThreadId").empty()) {
                const auto full = readCompleteItemText(client, scan, item, threadId, "messages", expected.size());
                const auto runId = str(item, "runId");
                const bool exact = full && *full == expected;
                if (!full || (runId.empty() && exact)) incomplete = true;
                if (exact && !runId.empty() && (launch || runId != command.target.runId))
                    matches[{threadId, runId}] = {threadId, runId, str(thread, "link")};
            }
            // This is a launch, so only its initial local user message could have created the Thread.
            return !launch && matches.size() < 2;
        });
        if (matches.size() > 1) break;
    }
    if (incomplete || matches.size() != 1)
        return result(Outcome::Unknown, matches.size() > 1 ? "More than one T3 Run matches the saved action" :
                      "T3 history did not establish exactly one Run for the saved action");
    const auto& match = matches.begin()->second;
    if ((!hint.empty() && hint != match.threadId) ||
        (!command.candidate.runId.empty() && command.candidate.runId != match.runId))
        return result(Outcome::Unknown, "The supplied T3 Run does not match the saved action");
    return result(Outcome::Accepted, {}, match);
}

struct Report { std::string text, itemId; bool truncated = false; };
Report readReport(Client& client, const RunRef& run, std::string_view view, std::string_view type) {
    Scan scan;
    Position before = 9007199254740991;
    for (;;) {
        scan.step();
        const auto page = client.call(ThreadRead, {{"threadId", run.threadId}, {"view", view}, {"limit", PageSize},
            {"runLimit", 1}, {"maxCharsPerItem", ExcerptChars}, {"beforePosition", before}});
        detail(page, run.threadId);
        const auto& items = array(page, "items");
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (!fromRun(*it, run) || str(*it, "type") != type || str(*it, "text").empty()) continue;
            auto text = itemText(*it);
            auto output = std::move(text.text);
            const auto id = str(*it, "itemId");
            while (text.more && output.size() < 8192) {
                scan.step();
                auto item = readItem(client, run.threadId, view, id, text.next);
                if (!fromRun(item, run) || str(item, "type") != type ||
                    str(item, "updatedAt") != str(*it, "updatedAt")) invalidResponse();
                text = itemText(item, text.next);
                output += text.text;
            }
            return {bounded(output, 8192), id, text.more || output.size() > 8192};
        }
        const auto next = cursor(page, "earlierPosition");
        if (!next) return {};
        if (*next >= before || items.empty() || *next != position(member(items.front(), "position"))) invalidResponse();
        before = *next;
    }
}

bool followUpReady(Client& client, const Command& command) {
    const auto settled = wait(client, command.target, true, 1);
    const auto effective = str(settled, "runId");
    const auto resultRun = str(settled.value("resultRun", Json::object()), "runId");
    if (boolean(settled, "timedOut") || !terminal(str(settled, "status")) || resultRun.empty() ||
        !ownsResult(client, command.target, effective, resultRun)) return false;
    const auto response = threadHead(client, command.target.threadId);
    const auto& info = detail(response, command.target.threadId);
    if (str(info, "projectId") != command.projectId || !member(info, "activeRunId").is_null() ||
        boolean(info, "archived") || position(member(info, "pendingRequestCount")) != 0 ||
        (str(info, "latestRunId") != effective && str(info, "latestRunId") != resultRun)) return false;
    // A held queue can exist even without an active Run.
    const auto queue = client.call("t3_queue", {{"action", "list"}, {"threadId", command.target.threadId}, {"limit", 1}});
    if (!array(queue, "items").empty()) return false;
    // Recheck after reading the queue. T3 v0.1.11 has no atomic expected-Run predicate on send.
    const auto checked = threadHead(client, command.target.threadId);
    const auto& current = detail(checked, command.target.threadId);
    return member(current, "activeRunId").is_null() && !boolean(current, "archived") &&
        str(current, "latestRunId") == str(info, "latestRunId") && position(member(current, "pendingRequestCount")) == 0;
}

Delivery steer(Client& client, const Command& command, bool& invoked) {
    const auto current = wait(client, command.target, false, 1);
    const auto effective = str(current, "runId");
    const auto head = threadHead(client, command.target.threadId);
    const auto& info = detail(head, command.target.threadId);
    if (!active(str(current, "status")) || str(info, "projectId") != command.projectId || boolean(info, "archived") ||
        member(info, "activeRunId") != Json(effective))
        return {Outcome::NotAccepted, {}, "The assigned Run is no longer running", true};
    invoked = true;
    const auto reply = client.call("t3_thread_send", {{"threadId", command.target.threadId}, {"mode", "steer"},
        {"message", command.instruction}, {"clientRequestId", command.actionId}});
    // T3 v0.1.11 cannot require the expected Run on send. If another Run of the task's Thread took over in
    // between, surface the mismatch without confirming the assigned Run.
    if (str(reply, "threadId") != command.target.threadId || str(reply, "delivery") != "steered") invalidResponse();
    RunRef received = command.target;
    received.runId = str(reply, "runId");
    if (received.runId != effective)
        return result(Outcome::Unknown, "T3 did not confirm steer delivery to " + effective +
                      (received.runId.empty() ? " (no Run ID returned)" : " (returned " + received.runId + ")"));
    return result(Outcome::Accepted, {}, std::move(received));
}

Delivery answer(Client& client, const Command& command) {
    const auto& thread = command.target.threadId;
    if (!pending(client, thread).contains(command.questionId)) return result(Outcome::QuestionGone);
    const auto watching = wait(client, command.target, false, 1);
    const auto runId = str(watching, "runId");
    // Codex's Run projection can stay "running" while its input activity
    // is "waiting". Attribution below checks the actual owned question.
    if (!active(str(watching, "status")))
        return result(Outcome::NotAccepted, "Use T3 to answer this question because the assigned Run is not waiting for input");
    auto attribution = attribute(client, {thread, runId, command.target.link}, command.questionId);
    if (attribution.kind == Attribution::Kind::Resolved) return result(Outcome::QuestionGone);
    if (attribution.kind != Attribution::Kind::Owned)
        return result(Outcome::NotAccepted, "Answer in T3 because the question could not be attributed to the assigned Run");
    Json answers;
    try { answers = sessionQuestionAnswers(attribution.fields, command.answers); }
    catch (const Conflict& error) { return result(Outcome::NotAccepted, error.what()); }
    try {
        if (readQuestion(client, thread, command.questionId) != attribution.raw)
            return result(Outcome::NotAccepted, "The T3 question changed before the saved answer could be sent");
        const auto responded = client.call(PendingRequest, {{"action", "respond"}, {"threadId", thread},
            {"requestId", command.questionId}, {"answers", answers}});
        position(member(responded, "sequence"));
        return result(Outcome::Accepted, {}, command.target);
    } catch (const Failure& error) {
        if (error.reason == Failure::Reason::QuestionGone) return result(Outcome::QuestionGone);
        throw;
    }
}

class Adapter final : public T3 {
public:
    explicit Adapter(std::filesystem::path directory) : directory_(std::move(directory)) {}

    std::string authGeneration(const std::string& environmentId, const Json& config) override {
        return credential(environmentId, config).generation;
    }

    std::optional<Delivery> preflight(const Command& command) override {
        try {
            verifyCommand(command);
            if (command.tool == "delegate") launchArgs(command);
            auto connection = credential(command.environmentId, command.config);
            requireOperate(connection);
            if (command.tool == "delegate") {
                Client client(std::move(connection));
                const auto project = client.call("t3_project_read",{{"projectId",command.projectId}});
                if (str(project,"id") != command.projectId)
                    return result(Outcome::Settings,"The configured ID does not identify the canonical T3 project");
            }
            return std::nullopt;
        } catch (const Failure& failure) {
            return failureDelivery(failure);
        }
    }

    Delivery send(const Command& command) override {
        bool invoked = false;
        try {
            verifyCommand(command);
            const bool launch = command.tool == "delegate";
            Json launchParameters;
            if (launch) launchParameters = launchArgs(command);
            auto connection = credential(command.environmentId, command.config);
            if (command.authGeneration.empty() || connection.generation != command.authGeneration)
                return {Outcome::Auth, {}, "The T3 sign-in changed before Flock sent this action", true};
            requireOperate(connection);
            const auto origin = checkedUrl(connection.mcpUrl).origin;
            Client client(std::move(connection));
            if (command.tool == "answer_session_question") return answer(client, command);
            if (command.tool == "steer") return steer(client, command, invoked);
            Json reply;
            if (launch) {
                invoked = true;
                reply = client.call("t3_thread_launch", std::move(launchParameters));
                if (str(reply, "projectId") != command.projectId) invalidResponse();
            } else {
                if (!followUpReady(client, command))
                    return result(Outcome::NotAccepted, "T3 has other work or the assigned Run could not be confirmed as the latest settled Run");
                invoked = true;
                reply = client.call("t3_thread_send", {{"threadId", command.target.threadId}, {"mode", "queue"},
                    {"message", sessionPrompt(command.actionId,command.instruction)}, {"clientRequestId", command.actionId}});
                const auto delivery = str(reply, "delivery");
                if (str(reply, "threadId") != command.target.threadId || str(reply, "runId") == command.target.runId ||
                    (delivery != "started" && delivery != "queued")) invalidResponse();
            }
            RunRef run{str(reply, "threadId"), str(reply, "runId"), str(reply, "link")};
            useLocalThreadLink(run,command.environmentId,origin);
            if (!hasRun(run)) return result(Outcome::Unknown, "T3 has not returned the assigned Run ID yet", std::move(run));
            return result(Outcome::Accepted, {}, std::move(run));
        } catch (const Failure& failure) {
            if (invoked) return workReply(failure);
            auto refused = failureDelivery(failure);
            if (command.tool != "answer_session_question") refused.definitelyUnsent = true;
            return refused;
        }
    }

    Delivery reconcile(const Command& command) override {
        if (command.tool == "steer") return result(Outcome::Unknown, "T3 cannot look up a steered note by action ID");
        try {
            verifyCommand(command);
            auto connection = credential(command.environmentId, command.config);
            const auto origin = checkedUrl(connection.mcpUrl).origin;
            Client client(std::move(connection));
            if (command.tool == "delegate" || command.tool == "follow_up") {
                auto recovered = reconcileWork(client, command);
                useLocalThreadLink(recovered.run,command.environmentId,origin);
                return recovered;
            }
            if ((!command.candidate.threadId.empty() && command.candidate.threadId != command.target.threadId) ||
                (!command.candidate.runId.empty() && command.candidate.runId != command.target.runId) ||
                (!command.candidateQuestionId.empty() && command.candidateQuestionId != command.questionId))
                return result(Outcome::Unknown, "The supplied T3 question is outside the saved answer destination");
            if (pending(client, command.target.threadId).contains(command.questionId))
                return result(Outcome::Unknown, "The T3 question is still pending. Confirm it in T3 before proceeding");
            return result(Outcome::QuestionGone);
        } catch (const Failure& failure) {
            return failureDelivery(failure);
        }
    }

    Observation observe(const std::string& environmentId, const Json& config, const RunRef& run) override {
        if (!hasRun(run)) return state(Observed::Unavailable, "The assigned T3 Run is missing");
        try {
            Client client(credential(environmentId, config));
            const auto reply = wait(client, run, true);
            const auto effective = str(reply, "runId");
            const auto status = str(reply, "status");
            const bool finished = terminal(status);
            if (finished && !boolean(reply, "timedOut")) {
                auto source = RunRef{run.threadId, effective, run.link};
                auto observed = state(Observed::Finished, status, true);
                const auto later = reply.value("resultRun", Json::object());
                const auto laterId = str(later, "runId");
                if (laterId.empty() || !terminal(str(later, "status"))) invalidResponse();
                if (ownsResult(client, run, effective, laterId)) {
                    source.runId = laterId;
                    observed.status = str(later, "status");
                }
                auto report = readReport(client, source, "messages", "assistant_message");
                if (observed.status != "completed") {
                    const auto error = readReport(client, source, "activity", "error");
                    observed.detail = "T3 Run ended as " + observed.status;
                    if (!error.text.empty()) {
                        observed.detail = bounded(error.text,2048);
                        if (report.itemId.empty()) report.itemId = error.itemId;
                        report.truncated = report.truncated || error.truncated;
                    }
                }
                if (report.text.empty()) report.text = "T3 Run " + observed.status + " without a written report.";
                observed.report = bounded(report.text, 8192);
                observed.truncated = report.truncated || observed.report.size() < report.text.size();
                observed.itemId = std::move(report.itemId);
                observed.resultRunId = source.runId;
                return observed;
            }
            if (!finished && !active(status)) invalidResponse();
            const auto head = threadHead(client, run.threadId);
            const auto& thread = detail(head, run.threadId);
            const auto count = position(member(thread, "pendingRequestCount"));
            if (count == 0) return state(finished ? Observed::Finished : Observed::Running, status);
            if (!finished && count == 1) {
                const auto requests = pending(client, run.threadId);
                if (requests.size() == 1) {
                    const auto& id = *requests.begin();
                    auto match = attribute(client, {run.threadId, effective, run.link}, id);
                    if (match.kind == Attribution::Kind::Owned) {
                        auto observed = state(Observed::Question, status);
                        observed.question = {id, std::move(match.fields)};
                        return observed;
                    }
                    if (match.kind == Attribution::Kind::Resolved) return state(Observed::Running, status);
                }
            }
            return state(Observed::ExternalInput, "Review pending approvals or questions in the T3 Thread");
        } catch (const Failure& failure) {
            if (failure.kind == Failure::Kind::Settings) return state(Observed::Unavailable, "settings");
            return state(Observed::Unavailable, failure.kind == Failure::Kind::Auth ? "auth" : failure.what());
        }
    }

    void interrupt(const std::string& environmentId, const Json& config, const RunRef& run) override {
        if (!hasRun(run)) throw Failure(Failure::Kind::Settings, "The assigned T3 Run is missing");
        auto connection = credential(environmentId, config);
        requireOperate(connection);
        Client client(std::move(connection));
        // An explicit wait can resolve T3's own restart continuation of the assigned Run.
        const auto current = wait(client, run, false, 1);
        const auto effective = str(current, "runId");
        if (terminal(str(current, "status"))) return;
        if (!active(str(current, "status"))) invalidResponse();
        const auto reply = client.call("t3_thread_interrupt", {{"threadId", run.threadId}, {"runId", effective}});
        const auto status = str(reply, "status");
        if (str(reply, "threadId") != run.threadId ||
            (status != "no_active_run" && (str(reply, "runId") != effective || (status != "interrupt_requested" && !terminal(status)))))
            invalidResponse();
    }

    Excerpt read(const ReadRequest& request) override {
        if (request.threadId.empty() || request.runId.empty() || (request.view != "activity" && request.view != "messages") ||
            (request.itemId.empty() && request.offset != 0) || request.offset > 9007199254740991ULL)
            throw Failure(Failure::Kind::Settings, "The T3 history read requires an owned Run and a valid view and offset");
        Client client(credential(request.environmentId, request.config));
        const RunRef run{request.threadId, request.runId, {}};
        Excerpt excerpt;
        excerpt.threadId = request.threadId;
        excerpt.runId = request.runId;
        const auto copy = [&](const Json& item, std::size_t offset) {
            auto page = itemText(item, offset);
            if (page.text.size() > 8192) invalidResponse();
            excerpt.text = std::move(page.text);
            excerpt.itemId = str(item, "itemId");
            excerpt.nextOffset = page.next;
            excerpt.more = page.more;
        };
        if (!request.itemId.empty()) {
            const auto item = readItem(client, request.threadId, request.view, request.itemId, request.offset);
            if (!fromRun(item, run)) throw Failure(Failure::Kind::Settings, "T3 history item does not belong to the requested Run");
            copy(item, request.offset);
            return excerpt;
        }
        Scan scan;
        walkThreadHistoryForward(client, scan, request.threadId, request.view, ExcerptChars, readCursor(request.cursor), [&](const Json& item) {
            if (!fromRun(item, run) || str(item, "text").empty()) return true;
            if (!excerpt.itemId.empty()) {
                excerpt.more = true;
                return false;
            }
            copy(item, 0);
            excerpt.nextCursor = std::to_string(position(member(item, "position")));
            return true;
        });
        return excerpt;
    }

    std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool includeMessages) override {
        return t3detail::sessionMetadata(directory_,target,includeMessages);
    }
    std::optional<SessionEventPage> sessionEvents(const SessionTarget& target, const SessionEventFilter& filter) override {
        return t3detail::sessionEvents(directory_,target,filter);
    }
    std::optional<SessionEvent> sessionEvent(const SessionTarget& target, std::string_view id) override {
        return t3detail::sessionEvent(directory_,target,id);
    }
    std::optional<SessionCounts> sessionCounts(const SessionTarget& target) override {
        return t3detail::sessionCounts(directory_,target);
    }
    std::optional<SessionAttachment> sessionAttachment(const SessionTarget& target, std::string_view item,
                                                       std::string_view attachment) override {
        return t3detail::sessionAttachment(directory_,target,item,attachment);
    }

private:
    std::filesystem::path directory_;

    Credential credential(const std::string& environment, const Json& config) const {
        auto connection = loadCredential(directory_, environment);
        if (!config.is_object() || (config.contains("t3") && !config["t3"].is_object()))
            throw Failure(Failure::Kind::Settings, "Invalid T3 Bot settings");
        if (config.contains("t3") && config["t3"].contains("mcpUrl")) {
            const auto hint = str(config["t3"], "mcpUrl");
            if (checkedUrl(hint).full != connection.mcpUrl)
                throw Failure(Failure::Kind::Settings, "The Bot's MCP URL differs from the T3 environment's OAuth connection");
        }
        return connection;
    }

    static void requireOperate(const Credential& connection) {
        if (!connection.operate) throw Failure(Failure::Kind::Auth, "Sign in again with a T3 Operate access level");
    }
};

}
}

std::unique_ptr<T3> makeT3Mcp(const std::filesystem::path& credentialDirectory) {
    return std::make_unique<t3detail::Adapter>(credentialDirectory);
}
}
