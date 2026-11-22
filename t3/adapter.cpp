#include "internal.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>

namespace flockv0 {
namespace t3detail {
namespace {
using Outcome = Delivery::Kind;
using Observed = Observation::Kind;
using Position = std::int64_t;
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
    if (error != std::errc() || end != text.data() + text.size() || parsed < 0 || parsed > 9007199254740991)
        throw Failure(Failure::Kind::Settings, "Invalid T3 history cursor");
    return parsed;
}

const Json& detail(const Json& response, const std::string& threadId) {
    const auto& thread = member(response, "thread");
    if (str(thread, "threadId") != threadId) invalidResponse();
    return thread;
}

Json threadHead(Client& client, const std::string& threadId, int runLimit = 1) {
    auto page = client.call(ThreadRead, {{"threadId", threadId}, {"view", "messages"},
                                         {"limit", 1}, {"maxCharsPerItem", 32}, {"runLimit", runLimit}});
    detail(page, threadId);
    return page;
}

bool fromRun(const Json& item, const RunRef& run) {
    return localItem(item, run.threadId) && str(item, "runId") == run.runId;
}

template<typename Visit>
void walkThreadHistoryForward(Client& client, Scan& scan, const std::string& threadId, std::string_view view,
          int maxChars, Position after, Visit visit) {
    for (;;) {
        scan.step();
        const auto page = client.call(ThreadRead, {{"threadId", threadId}, {"view", view}, {"limit", PageSize},
            {"runLimit", 1}, {"maxCharsPerItem", maxChars}, {"afterPosition", after}});
        detail(page, threadId);
        const auto& items = array(page, "items");
        const auto start = after;
        for (const auto& item : items) {
            const auto next = position(member(item, "position"));
            if (next <= after) invalidResponse();
            after = next;
            if (!visit(item)) return;
        }
        if (!boolean(page, "hasMore")) return;
        if (after <= start || cursor(page, "nextPosition") != after) invalidResponse();
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
    if (itemId.empty() || offset > 9007199254740991ULL) invalidResponse();
    const auto response = client.call(ThreadRead, {{"threadId", threadId}, {"view", view}, {"limit", 1},
        {"runLimit", 1}, {"maxCharsPerItem", chars}, {"itemId", itemId}, {"textOffset", offset}});
    detail(response, threadId);
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

// A delegated completion is reported thread-wide by T3 wait. In v0.1.11 a
// completion wake can be a typed notification with the child's title, with
// no task ID in that item. Require a unique settled child in the already
// owned chain and reject any intervening Run with an ordinary prompt.
bool ownsResult(Client& client, const RunRef& requested, const std::string& effectiveRunId,
                const std::string& resultRunId) {
    if (resultRunId == effectiveRunId || resultRunId == requested.runId) return true;
    const auto head = threadHead(client, requested.threadId, 50);
    const auto& runs = array(head, "recentRuns");
    std::map<Position, std::string> ordered;
    Position base = 0, ending = 0;
    for (const auto& run : runs) {
        const auto id = str(run, "runId");
        const auto ordinal = position(member(run, "ordinal"));
        if (id.empty() || !ordered.emplace(ordinal, id).second) invalidResponse();
        if (id == effectiveRunId) base = ordinal;
        if (id == resultRunId) ending = ordinal;
    }
    if (base == 0 || ending <= base) return false;
    std::set<std::string> candidates;
    for (auto ordinal = base + 1; ordinal <= ending; ++ordinal) {
        const auto found = ordered.find(ordinal);
        if (found == ordered.end()) return false;
        candidates.insert(found->second);
    }
    struct Child {
        std::string title, updatedAt;
    };
    struct Trigger {
        std::string taskId, notice, updatedAt;
    };
    std::map<std::string, std::set<std::string>> subtasks;
    std::map<std::string,Child> children;
    std::map<std::string, std::vector<Trigger>> triggers;
    std::set<std::string> ordinaryInputs;
    Scan scan;
    walkThreadHistoryForward(client, scan, requested.threadId, "activity", 700, -1, [&](const Json& item) {
        if (!localItem(item, requested.threadId)) return true;
        const auto run = str(item, "runId");
        const auto type = str(item, "type");
        if (type == "subagent") {
            const auto id = str(item, "subagentId");
            const auto title = str(item, "title");
            if (!id.empty()) {
                subtasks[run].insert(id);
                if (!title.empty())
                    children.emplace(id,Child{title,terminal(str(item,"status")) ? str(item,"updatedAt") : ""});
            }
        }
        if (!candidates.contains(run)) return true;
        if (type == "notification") {
            const auto message = str(item,"text");
            constexpr std::string_view header = "Delegated task \"";
            constexpr std::string_view footer = "\" finished";
            if (str(item,"status") == "completed" && !boolean(item,"textTruncated") &&
                message.starts_with(header) && message.ends_with(footer) && message.size() > header.size()+footer.size()) {
                triggers[run].push_back({{},message.substr(header.size(),message.size()-header.size()-footer.size()),
                                         str(item,"updatedAt")});
            } else ordinaryInputs.insert(run);
        }
        if (type == "user_message") {
            const auto text = str(item, "text");
            constexpr std::string_view prefix = "Delegated task ";
            std::string taskId;
            if (str(item, "createdBy") == "agent" && str(item, "creationSource") == "server" &&
                text.starts_with(prefix) && !text.starts_with("Delegated task \"")) {
                const auto end = text.find_first_of(" \r\n\t", prefix.size());
                if (end != std::string::npos) taskId = text.substr(prefix.size(), end - prefix.size());
            }
            if (taskId.empty()) ordinaryInputs.insert(run);
            else triggers[run].push_back({std::move(taskId),{},{}});
        }
        return true;
    });
    auto tasks = subtasks[requested.runId];
    tasks.merge(subtasks[effectiveRunId]);
    std::set<std::string> delivered;
    for (auto ordinal = base + 1; ordinal <= ending; ++ordinal) {
        const auto& id = ordered.at(ordinal);
        if (!triggers.contains(id) || ordinaryInputs.contains(id)) return false;
        for (const auto& trigger : triggers.at(id)) {
            auto childId = trigger.taskId;
            if (!trigger.notice.empty()) {
                if (trigger.updatedAt.empty()) return false;
                for (const auto& [id,child] : children) {
                    if (!sameDelegateTitle(child.title,trigger.notice)) continue;
                    if (!childId.empty() || child.updatedAt.empty() || child.updatedAt > trigger.updatedAt) return false;
                    childId = id;
                }
            }
            if (!tasks.contains(childId) || !delivered.insert(childId).second) return false;
        }
        tasks.merge(subtasks[id]);
    }
    return true;
}

bool questionGone(const Failure& error) {
    return error.code == "invalid_request" &&
        (error.serverMessage == "The pending user-input request was not found." ||
         error.serverMessage == "The user-input request was already answered." ||
         error.serverMessage.starts_with("The user-input request is no longer pending ("));
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
        if (id.empty() || !ids.insert(id).second || trim(str(question, "question")).empty() ||
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
            const auto value = option.contains("value") ? str(option, "value") : trim(label);
            if (trim(label).empty() || trim(value).empty() || !values.insert(value).second) return {};
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
        if (questionGone(error)) return {Attribution::Kind::Resolved, {}, {}};
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
        if (failure.kind == Failure::Kind::Auth) throw;
        return {Attribution::Kind::Ambiguous, {}, {}};
    }
    if (unverified || matching != 1 || !exactOwner) return {Attribution::Kind::Ambiguous, {}, {}};
    return {Attribution::Kind::Owned, std::move(fields), std::move(questions)};
}

std::optional<Json> wireAnswers(const Json& fields, const Json& answers) {
    if (!answers.is_object()) return std::nullopt;
    auto result = Json::object();
    std::size_t consumed = 0;
    for (const auto& field : fields) {
        const auto id = str(field, "id");
        const auto answer = answers.find(id);
        const auto& options = field["options"];
        if (answer == answers.end()) {
            if (field["required"].get<bool>()) return std::nullopt;
            if (options.empty()) result[id] = "";
            continue;
        }
        ++consumed;
        auto selected = answer->is_array() ? *answer : Json::array({*answer});
        if (selected.empty() || (!field["multiple"].get<bool>() && selected.size() != 1)) return std::nullopt;
        std::set<std::string> seen;
        bool custom = false;
        for (const auto& value : selected) {
            if (!value.is_string()) return std::nullopt;
            const auto& text = value.get_ref<const std::string&>();
            if (trim(text).empty() || text.size() > 8000 || !seen.insert(text).second) return std::nullopt;
            const bool offered = std::any_of(options.begin(), options.end(), [&](const Json& option) {
                return option["value"] == value;
            });
            if (!offered) {
                if (!field["freeform"].get<bool>() || selected.size() != 1) return std::nullopt;
                custom = true;
            }
        }
        result[id] = field["multiple"].get<bool>() && !custom ? selected : selected.front();
    }
    if (consumed != answers.size()) return std::nullopt;
    return result;
}

std::string actionPrompt(const Command& command) {
    return "[Flock action " + command.actionId + "]\n\n" + command.instruction + "\n\n[/Flock action]";
}

void useLocalThreadLink(RunRef& run, std::string_view environment, std::string_view origin) {
    // checkedUrl permits plaintext HTTP only on loopback. A local-only T3 environment
    // opens in its own browser client, where the user authorized this connection.
    if (origin.starts_with("http://") && !run.threadId.empty())
        run.link = std::string(origin) + "/" + encode(environment) + "/" + encode(run.threadId);
}

Json launchArgs(const Command& command) {
    Json args = {{"projectId", command.projectId}, {"title", "Flock task"},
                 {"message", actionPrompt(command)}, {"clientRequestId", command.actionId}};
    if (!command.config.is_object()) throw Failure(Failure::Kind::Settings, "Invalid T3 Bot settings");
    const auto settings = command.config.value("session", Json::object());
    if (!settings.is_object()) throw Failure(Failure::Kind::Settings, "Invalid T3 Bot settings");
    for (const auto field : {"runtimeMode", "interactionMode", "workspaceStrategy"}) {
        if (settings.contains(field) && !settings[field].is_null()) args[field] = settings[field];
    }
    if (args.contains("runtimeMode")) {
        const auto mode = str(args, "runtimeMode");
        if (mode != "approval-required" && mode != "auto-accept-edits" && mode != "auto" && mode != "full-access")
            throw Failure(Failure::Kind::Settings, "Invalid T3 runtime mode");
    }
    if (args.contains("interactionMode") && args["interactionMode"] != "default" && args["interactionMode"] != "plan")
        throw Failure(Failure::Kind::Settings, "Invalid T3 interaction mode");
    if (settings.contains("providerInstanceId") || settings.contains("model")) {
        const auto provider = str(settings, "providerInstanceId");
        const auto model = str(settings, "model");
        if (trim(provider).empty() || trim(model).empty())
            throw Failure(Failure::Kind::Settings, "Specify both the T3 provider instance and Session model");
        args["modelSelection"] = {{"instanceId", provider}, {"model", model}};
    }
    if (args.contains("workspaceStrategy")) {
        const auto& workspace = args["workspaceStrategy"];
        const auto type = str(workspace, "type");
        if (type != "root" && (type != "worktree" || trim(str(workspace, "baseRef")).empty()) &&
            (type != "existing_worktree" || trim(str(workspace, "worktreePath")).empty()))
            throw Failure(Failure::Kind::Settings, "Invalid T3 workspace strategy");
    }
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
        actionPrompt(command).size() > 120000)
        throw Failure(Failure::Kind::Settings, "Invalid T3 command, action ID or destination");
}

Delivery failureDelivery(const Failure& failure) {
    if (failure.kind == Failure::Kind::Auth) return result(Outcome::Auth, failure.what());
    if (failure.kind == Failure::Kind::Settings) return result(Outcome::Settings, failure.what());
    return result(Outcome::Unknown, failure.what());
}

Delivery workReply(const Failure& failure) {
    if (failure.kind == Failure::Kind::Auth) {
        auto refused = failureDelivery(failure);
        refused.definitelyUnsent = true;
        return refused;
    }
    if (failure.code == "model_unavailable" || failure.code == "provider_unavailable" ||
        (failure.code == "invalid_request" && (failure.serverMessage.starts_with("Pass modelSelection:") ||
          failure.serverMessage == "The project was not found." ||
          failure.serverMessage.find("No thread was created.") != std::string::npos)))
        return {Outcome::Settings, {}, "T3 rejected the project, model, or workspace settings before starting a Run", true};
    return failureDelivery(failure);
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
    const auto expected = actionPrompt(command);
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
                if (!full || (runId.empty() && *full == expected)) incomplete = true;
                if (full && *full == expected && !runId.empty() && (launch || runId != command.target.runId))
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

// Delivers a note into the assigned Run while it is still running. It never starts another Run.
Delivery steer(Client& client, const Command& command, bool& invoked) {
    // Like interrupt, an explicit wait resolves T3's own restart continuation of the assigned Run.
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
    // between, report the Run that actually received the note.
    if (str(reply, "threadId") != command.target.threadId || str(reply, "delivery") != "steered") invalidResponse();
    RunRef received = command.target;
    received.runId = effective;
    if (const auto runId = str(reply, "runId"); !runId.empty()) received.runId = runId;
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
    const auto answers = wireAnswers(attribution.fields, command.answers);
    if (!answers) return result(Outcome::NotAccepted, "The saved answers do not satisfy the T3 question fields");
    try {
        if (readQuestion(client, thread, command.questionId) != attribution.raw)
            return result(Outcome::NotAccepted, "The T3 question changed before the saved answer could be sent");
        const auto responded = client.call(PendingRequest, {{"action", "respond"}, {"threadId", thread},
            {"requestId", command.questionId}, {"answers", *answers}});
        position(member(responded, "sequence"));
        return result(Outcome::Accepted, {}, command.target);
    } catch (const Failure& error) {
        if (questionGone(error)) return result(Outcome::QuestionGone);
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
            auto connection = credential(command.environmentId, command.config);
            requireOperate(connection);
            if (command.tool == "delegate") {
                launchArgs(command);
                Client client(std::move(connection));
                const auto project = client.call("t3_project_read",{{"projectId",command.projectId}});
                if (str(project,"id") != command.projectId)
                    return result(Outcome::Settings,"The configured ID does not identify the canonical T3 project");
            }
            return std::nullopt;
        } catch (const Failure& failure) {
            if (failure.code == "invalid_request" && failure.serverMessage == "The project was not found.")
                return result(Outcome::Settings,"The configured T3 project was not found");
            return failureDelivery(failure);
        }
    }

    Delivery send(const Command& command) override {
        bool invoked = false;
        try {
            verifyCommand(command);
            auto connection = credential(command.environmentId, command.config);
            if (command.authGeneration.empty() || connection.generation != command.authGeneration)
                return {Outcome::Auth, {}, "The T3 sign-in changed before Flock sent this action", true};
            requireOperate(connection);
            const auto origin = checkedUrl(connection.mcpUrl).origin;
            Client client(std::move(connection));
            if (command.tool == "answer_session_question") return answer(client, command);
            if (command.tool == "steer") return steer(client, command, invoked);
            const bool launch = command.tool == "delegate";
            Json reply;
            if (launch) {
                auto args = launchArgs(command);
                invoked = true;
                reply = client.call("t3_thread_launch", std::move(args));
                if (str(reply, "projectId") != command.projectId) invalidResponse();
            } else {
                if (!followUpReady(client, command))
                    return result(Outcome::NotAccepted, "T3 has other work or the assigned Run could not be confirmed as the latest settled Run");
                invoked = true;
                reply = client.call("t3_thread_send", {{"threadId", command.target.threadId}, {"mode", "queue"},
                    {"message", actionPrompt(command)}, {"clientRequestId", command.actionId}});
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
        try {
            verifyCommand(command);
            auto connection = credential(command.environmentId, command.config);
            const auto origin = checkedUrl(connection.mcpUrl).origin;
            Client client(std::move(connection));
            if (command.tool != "answer_session_question") {
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
            if (terminal(status) && !boolean(reply, "timedOut")) {
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
            if (!active(status) && !terminal(status)) invalidResponse();
            const auto head = threadHead(client, run.threadId);
            const auto& thread = detail(head, run.threadId);
            const auto count = position(member(thread, "pendingRequestCount"));
            if (count != 0) {
                const auto requests = pending(client, run.threadId);
                if (count == 1 && requests.size() == 1 && active(status)) {
                    const auto& id = *requests.begin();
                    auto match = attribute(client, {run.threadId, effective, run.link}, id);
                    if (match.kind == Attribution::Kind::Owned) {
                        auto observed = state(Observed::Question, status);
                        observed.question = {id, std::move(match.fields)};
                        return observed;
                    }
                    if (match.kind == Attribution::Kind::Resolved) return state(Observed::Running, status);
                }
                return state(Observed::ExternalInput, "Review pending approvals or questions in the T3 Thread");
            }
            return terminal(status) ? state(Observed::Finished, status) : state(Observed::Running, status);
        } catch (const Failure& failure) {
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

    std::optional<SessionMetadata> findSession(const SessionTarget& target, std::string_view query) override {
        return t3detail::findSession(directory_,target,query);
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
        return readCredential(directory_, environment, config);
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
