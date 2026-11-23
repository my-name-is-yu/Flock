#include "internal.hpp"
#include "../src/commit_check.hpp"
#include "flock/session_read.hpp"
#include "flock/session_settings.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <thread>
#include <utility>

namespace flockv0 {
namespace t3detail {
namespace {
using Outcome = Delivery::Kind;
using Observed = Observation::Kind;
using Position = Id;
using Clock = std::chrono::steady_clock;
constexpr int ExcerptChars = 2730; // T3 uses UTF-16 offsets. 2730 code units occupy at most 8190 UTF-8 bytes.
constexpr int PageSize = 30;
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

Observation state(Observed kind, std::string status) {
    Observation observation;
    observation.kind = kind;
    observation.status = std::move(status);
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

Json threadHead(Client& client, const std::string& threadId, int runLimit = 1) {
    return readThread(client,threadId,"messages",1,32,Json::object(),runLimit);
}

void checkVerificationRoute(Client& client, const Json& config, std::string_view environment,
                            const std::string& thread = {}) {
    if (!config.contains("commitVerification")) return;
    const auto& route = config.at("commitVerification");
    const auto info = client.call("t3_environment_read",Json::object());
    if (str(info,"environmentId") != environment)
        throw Failure(Failure::Kind::Settings,"The OAuth endpoint belongs to a different T3 environment");
    if (!thread.empty() && str(threadHead(client,thread).at("thread"),"projectId") != str(route,"projectId"))
        throw Failure(Failure::Kind::Settings,"The Run's Session is outside the saved T3 project");
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

enum class WaitFor { Run, DelegatedWork };

Json wait(Client& client, const RunRef& run, WaitFor scope, int timeout = 400) {
    const auto response = client.call("t3_thread_wait", {{"threadId", run.threadId}, {"runId", run.runId},
        {"timeoutMs", timeout}, {"untilDelegatedWorkSettled", scope == WaitFor::DelegatedWork}});
    if (str(response, "threadId") != run.threadId || str(response, "runId").empty()) invalidResponse();
    boolean(response, "timedOut");
    return response;
}

bool isRestartContinuation(Client& client, const RunRef& assigned, const std::string& effective) {
    // T3 creates a server message keyed by the cut Run each time it resumes after a restart.
    constexpr std::string_view prefix = "message:restart-continuation:";
    Scan scan;
    auto current = assigned.runId;
    return !walkThreadHistoryForward(client,scan,assigned.threadId,"messages",32,-1,[&](const Json& item) {
        if (localItem(item,assigned.threadId) && str(item,"type") == "user_message" &&
            str(item,"creationSource") == "server" && str(item,"messageId") == std::string(prefix) + current) {
            current = str(item,"runId");
            if (current.empty()) invalidResponse();
        }
        return current != effective;
    });
}

bool confirmsQueueCancellation(Client& client, const RunRef& assigned, const Json& head) {
    for (const auto& run : array(head,"recentRuns")) {
        if (str(run,"runId") != assigned.runId) continue;
        const auto started = run.find("startedAt");
        if (str(run,"status") != "cancelled" || started == run.end() || !started->is_null()) return false;
        const auto exact = wait(client,assigned,WaitFor::Run,1);
        return str(exact,"runId") == assigned.runId && str(exact,"status") == "cancelled" &&
               !boolean(exact,"timedOut");
    }
    return false;
}

struct CompletionNotice { std::size_t count; std::string updatedAt; };
struct DelegatedChild {
    std::string parentRun, settledAt;
    bool consumed = false;
};

std::optional<CompletionNotice> completionNotice(const Json& item) {
    const auto text = str(item,"text");
    const auto updatedAt = str(item,"updatedAt");
    if (str(item,"status") != "completed" || boolean(item,"textTruncated") || updatedAt.empty())
        return std::nullopt;

    constexpr std::string_view single = "Delegated task ";
    const auto verb = [](std::string_view value) {
        return value == "finished" || value == "failed" || value == "stopped";
    };
    std::string_view frame = text;
    if (frame.starts_with(single)) {
        frame.remove_prefix(single.size());
        if (verb(frame)) return CompletionNotice{1,updatedAt};
        const auto endQuote = frame.rfind('"');
        if (frame.starts_with('"') && endQuote > 1 && endQuote != frame.npos &&
            frame.substr(endQuote + 1).starts_with(' ') && verb(frame.substr(endQuote + 2)))
            return CompletionNotice{1,updatedAt};
        return std::nullopt;
    }

    constexpr std::string_view batch = " delegated tasks ";
    const auto countEnd = frame.find(batch);
    if (countEnd == frame.npos) return std::nullopt;
    const auto counts = frame.substr(0,countEnd);
    const auto outcome = frame.substr(countEnd + batch.size());
    if (!verb(outcome.substr(0,outcome.find(": ")))) return std::nullopt;

    const auto parseCount = [](std::string_view value, std::size_t& count) {
        const auto end = value.data() + value.size();
        const auto result = std::from_chars(value.data(),end,count);
        return result.ec == std::errc{} && result.ptr == end && count > 0;
    };
    const auto of = counts.find(" of ");
    std::size_t count = 0, total = 0;
    if (!parseCount(counts.substr(0,of),count) ||
        (of != counts.npos && (!parseCount(counts.substr(of + 4),total) || count > total)))
        return std::nullopt;
    return CompletionNotice{count,updatedAt};
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
            // Provider-native subagents also appear as subagent items but never produce delegated-task
            // notices. Count only app-owned delegated tasks, whose node ids T3 derives with this prefix.
            if (id.starts_with("node:delegated-task:"))
                children.emplace(id,DelegatedChild{run,terminal(str(item,"status")) ? str(item,"updatedAt") : ""});
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

    std::vector<DelegatedChild*> settled;
    for (auto& [id,child] : children)
        if (!child.settledAt.empty()) settled.push_back(&child);
    // Preserve subagent ID order for children with the same settlement time.
    std::stable_sort(settled.begin(),settled.end(),[](const auto* a, const auto* b) {
        return a->settledAt < b->settledAt;
    });

    std::set<std::string> ownedRuns{requested.runId,effectiveRunId};
    for (auto ordinal = base + 1; ordinal <= ending; ++ordinal) {
        const auto& run = ordered.at(ordinal);
        const auto& inputs = notices.at(run);
        if (inputs.empty()) return false;
        for (const auto& notice : inputs) {
            auto remaining = notice.count;
            for (auto* child : settled) {
                if (child->settledAt > notice.updatedAt) break;
                if (child->consumed || !ownedRuns.contains(child->parentRun)) continue;
                child->consumed = true;
                if (--remaining == 0) break;
            }
            if (remaining != 0) return false;
        }
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

struct LaunchArguments {
    Json parameters;
    std::string pin;
};

LaunchArguments launchArgs(const Command& command) {
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
    std::optional<std::string> pin;
    if (const auto workspace = settings.find("workspaceStrategy");
        workspace != settings.end() && workspace->contains("expectedCommit")) {
        pin = str(*workspace,"expectedCommit");
        workspace->erase("expectedCommit");
    }
    Json args;
    try { args = normalizeSessionSettings(settings); }
    catch (const Conflict& error) { throw Failure(Failure::Kind::Settings, error.what()); }
    if (pin) {
        const auto& workspace = args.at("workspaceStrategy");
        if (!gitCommit(*pin) || str(workspace,"type") != "worktree" || str(workspace,"baseRef") != *pin ||
            workspace.value("startFromOrigin",Json(nullptr)) != Json(false) ||
            str(args,"runtimeMode") != "approval-required" || command.actionId.size() > 250)
            throw Failure(Failure::Kind::Settings, "Invalid pinned specialist workspace");
    }
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
    if (command.sessionOrigin) {
        const auto& origin = *command.sessionOrigin;
        args["delegatedFrom"] = {{"environmentId",origin.target.environmentId},{"threadId",origin.target.threadId},
                                 {"title",origin.title}};
    }
    return {std::move(args),std::move(pin).value_or(std::string{})};
}

void verifyCommand(const Command& command) {
    if (command.tool == "answer_session_question") {
        if (!hasRun(command.target) || command.questionId.empty() || !command.answers.is_object())
            throw Failure(Failure::Kind::Settings, "The T3 answer requires a Thread, Run, question ID and answers");
        return;
    }
    const bool validTarget = command.tool == "delegate" ||
        ((command.tool == "start_in_session" || command.tool == "message_session") &&
         !command.target.threadId.empty() && command.target.runId.empty()) ||
        (command.tool == "follow_up" && hasRun(command.target));
    if (!validTarget || (command.tool != "message_session" && command.projectId.empty()) ||
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

std::vector<std::string> projectThreads(Client& client, Scan& scan, const Command& command) {
    std::vector<std::string> found;
    std::set<std::string> visited;
    std::optional<Position> total;
    Position offset = 0;
    for (;;) {
        scan.step();
        const auto page = client.call("t3_thread_list", {{"projectId", command.projectId},
                                                         {"includeSubagents", command.sessionOrigin.has_value()},
                                                         {"limit", 50}, {"cursor", offset}});
        if (str(page, "projectId") != command.projectId) invalidResponse();
        const auto count = position(member(page, "total"));
        if (total && count != *total) invalidResponse();
        total = count;
        const auto& threads = array(page, "threads");
        for (const auto& thread : threads) {
            const auto id = str(thread, "threadId");
            if (id.empty() || str(thread, "projectId") != command.projectId || !visited.insert(id).second) invalidResponse();
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

void queuedMatches(Client& client, Scan& scan, const std::string& thread,
                   const std::string& expected, std::set<std::string>& matches) {
    Position offset = 0;
    for (;;) {
        scan.step();
        const auto page = client.call("t3_queue",{{"action","list"},{"threadId",thread},{"limit",50},{"cursor",offset}});
        for (const auto& item : array(page,"items")) {
            const auto run = str(item,"queuedRunId");
            const auto text = str(item,"text");
            if (run.empty() || !member(item,"text").is_string()) invalidResponse();
            if (!boolean(item,"truncated")) {
                if (text == expected) matches.insert(run);
            } else if (expected.starts_with(text)) {
                scan.step();
                try {
                    const auto entry = client.call("t3_queue",{{"action","read"},{"threadId",thread},{"queuedRunId",run}});
                    if (str(entry,"queuedRunId") != run || !member(entry,"text").is_string()) invalidResponse();
                    if (!boolean(entry,"truncated") && str(entry,"text") == expected) matches.insert(run);
                } catch (const Failure& error) {
                    // The queued input may have started while it was being read. Check history below.
                    if (error.kind != Failure::Kind::Tool) throw;
                }
            }
        }
        const auto next = cursor(page,"nextCursor");
        if (!next || matches.size() > 1) return;
        if (*next <= offset || *next != offset + 50) invalidResponse();
        offset = *next;
    }
}

Delivery reconcileWork(Client& client, const Command& command) {
    const bool launch = command.tool == "delegate";
    const auto& destination = launch ? command.launchReceiptThreadId : command.target.threadId;
    const auto& hint = command.candidate.threadId;
    if (!destination.empty() && !hint.empty() && hint != destination)
        return result(Outcome::Unknown, "The supplied Thread is outside the saved action destination");
    Scan scan;
    auto threads = destination.empty() ? projectThreads(client, scan, command) : std::vector{destination};
    if (destination.empty() && !hint.empty() && std::find(threads.begin(), threads.end(), hint) == threads.end())
        threads.push_back(hint);
    const auto expected = sessionPrompt(command.actionId,command.instruction);
    std::map<std::pair<std::string, std::string>, RunRef> matches;
    bool incomplete = false;
    for (const auto& threadId : threads) {
        scan.step();
        const auto head = threadHead(client, threadId);
        const auto& thread = head.at("thread");
        if (str(thread, "projectId") != command.projectId ||
            (launch && (str(thread, "createdBy") != "agent" || str(thread, "creationSource") != "mcp"))) continue;
        if (command.tool == "start_in_session") {
            std::set<std::string> queued;
            queuedMatches(client,scan,threadId,expected,queued);
            for (const auto& run : queued)
                matches[{threadId,run}] = {threadId,run,str(thread,"link")};
        }
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

struct Report {
    std::string text, itemId, sessionCreatedAt;
    bool truncated = false;
};
Report readReport(Client& client, const RunRef& run, std::string_view view, std::string_view type) {
    Scan scan;
    Report report;
    Position before = maxHistoryPosition;
    for (;;) {
        scan.step();
        const auto page = readThread(client,run.threadId,view,PageSize,ExcerptChars,{{"beforePosition",before}});
        if (before == maxHistoryPosition) {
            auto createdAt = str(page.at("thread"),"createdAt");
            if (session_read::utcTimestamp(createdAt)) report.sessionCreatedAt = std::move(createdAt);
        }
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
            report.text = bounded(output, 8192);
            report.itemId = id;
            report.truncated = text.more || output.size() > 8192;
            return report;
        }
        const auto next = cursor(page, "earlierPosition");
        if (!next) return report;
        if (*next >= before || items.empty() || *next != position(member(items.front(), "position"))) invalidResponse();
        before = *next;
    }
}

std::string_view unwrappedCommand(std::string_view input) {
    const auto space = input.find(' ');
    const auto shell = input.substr(0,space);
    if (space == input.npos || (shell != "/bin/zsh" && shell != "/bin/bash" && shell != "/bin/sh"))
        return input;
    auto args = input.substr(space);
    if (args.starts_with(" -lc ")) args.remove_prefix(5);
    else if (args.starts_with(" -c ")) args.remove_prefix(4);
    else return input;
    if (args.size() < 2 || (args.front() != '\'' && args.front() != '"') || args.back() != args.front())
        return input;
    return args.substr(1,args.size()-2);
}

std::string commitCheckOutput(const Json& item, std::string_view command) {
    if (str(item,"status") != "completed" || boolean(item,"textTruncated")) return {};
    const auto text = str(item,"text");
    const auto newline = text.find('\n');
    if (newline == text.npos || !text.starts_with("$ ") ||
        unwrappedCommand(std::string_view(text).substr(2,newline-2)) != command) return {};
    const auto output = trimSessionQuestionText(std::string_view(text).substr(newline+1));
    return gitCommit(output) ? std::string(output) : std::string{};
}

std::string checkedCommit(Client& client, const RunRef& assigned, const RunRef& effective,
                          const RunRef& result, const RunEvidence& report, std::string_view expected) {
    if (!gitCommit(expected) || report.itemId.empty()) return {};
    Scan scan;
    std::string before, after;
    bool sawCommand = false, finalReport = false;
    // The initial command belongs to the assigned work. The final command and report belong to its result.
    walkThreadHistoryForward(client,scan,assigned.threadId,"activity",512,-1,[&](const Json& item) {
        const bool fromResult = fromRun(item,result);
        const bool fromWork = fromRun(item,assigned) || fromRun(item,effective);
        if (!fromResult && !fromWork) return true;
        const auto type = str(item,"type");
        if (type == "command_execution") {
            if (!std::exchange(sawCommand,true) && fromWork)
                before = commitCheckOutput(item,commit_check::before);
            after = fromResult ? commitCheckOutput(item,commit_check::after) : std::string{};
            finalReport = false;
        } else if (fromResult && type == "assistant_message") {
            finalReport = str(item,"itemId") == report.itemId && str(item,"status") == "completed" && !after.empty();
        }
        return true;
    });
    if (!before.empty() && before != expected) return before;
    return before == expected && finalReport ? after : std::string{};
}

RunEvidence unavailableReport(const Failure& failure) {
    RunEvidence evidence;
    evidence.availability = RunEvidence::Availability::ReportUnavailable;
    evidence.detail = "Could not read the T3 Run report: " + std::string(failure.what());
    return evidence;
}

std::optional<RunEvidence> pinnedEvidenceProblem(Client& client, const Json& config, const RunRef& source,
                                                std::string_view status, std::string_view origin) {
    if (status != "completed") return {};
    const auto settings = config.value("session",Json::object());
    const auto workspace = settings.find("workspaceStrategy");
    if (workspace == settings.end() || !workspace->contains("expectedCommit")) return {};
    const auto pin = str(*workspace,"expectedCommit");
    RunEvidence problem;
    problem.availability = RunEvidence::Availability::ReportUnavailable;
    problem.detail = "Could not verify the specialist's pinned workspace";
    if (!gitCommit(pin) || !loopbackGitHost(origin)) return problem;
    const auto head = threadHead(client,source.threadId);
    const auto& thread = head.at("thread");
    const auto actual = workspaceHead(client,str(thread,"projectId"),thread);
    if (!actual) return problem;
    if (actual->clean && actual->commit == pin) return {};
    problem.availability = RunEvidence::Availability::Available;
    problem.detail = "The specialist worktree changed from the saved commit";
    problem.report = problem.detail;
    return problem;
}

RunEvidence readTerminalEvidence(Client& client, const RunRef& source, std::string_view status) {
    Report report;
    try {
        report = readReport(client, source, "messages", "assistant_message");
    } catch (const Failure& failure) {
        return unavailableReport(failure);
    }

    RunEvidence evidence;
    if (status != "completed") {
        evidence.detail = "T3 Run ended as " + std::string(status);
        try {
            const auto error = readReport(client, source, "activity", "error");
            if (!error.text.empty()) {
                evidence.detail = bounded(error.text,2048);
                if (report.itemId.empty()) report.itemId = error.itemId;
                report.truncated = report.truncated || error.truncated;
            }
        } catch (const Failure& failure) {
            evidence.availability = RunEvidence::Availability::DiagnosticUnavailable;
            evidence.detail += ". Could not read T3 error history: " + std::string(failure.what());
        }
    }
    if (report.text.empty()) report.text = "T3 Run " + std::string(status) + " without a written report.";
    evidence.report = bounded(report.text, 8192);
    evidence.truncated = report.truncated || evidence.report.size() < report.text.size();
    evidence.itemId = std::move(report.itemId);
    evidence.sessionCreatedAt = std::move(report.sessionCreatedAt);
    return evidence;
}

bool followUpReady(Client& client, const Command& command) {
    const auto settled = wait(client, command.target, WaitFor::DelegatedWork, 1);
    const auto effective = str(settled, "runId");
    const auto resultRun = str(settled.value("resultRun", Json::object()), "runId");
    if (boolean(settled, "timedOut") || !terminal(str(settled, "status")) || resultRun.empty() ||
        !ownsResult(client, command.target, effective, resultRun)) return false;
    const auto response = threadHead(client, command.target.threadId);
    const auto& info = response.at("thread");
    if (str(info, "projectId") != command.projectId || !member(info, "activeRunId").is_null() ||
        boolean(info, "archived") || position(member(info, "pendingRequestCount")) != 0 ||
        (str(info, "latestRunId") != effective && str(info, "latestRunId") != resultRun)) return false;
    // A held queue can exist even without an active Run.
    const auto queue = client.call("t3_queue", {{"action", "list"}, {"threadId", command.target.threadId}, {"limit", 1}});
    if (!array(queue, "items").empty()) return false;
    // Recheck after reading the queue. T3 v0.1.11 has no atomic expected-Run predicate on send.
    const auto checked = threadHead(client, command.target.threadId);
    const auto& current = checked.at("thread");
    return member(current, "activeRunId").is_null() && !boolean(current, "archived") &&
        str(current, "latestRunId") == str(info, "latestRunId") && position(member(current, "pendingRequestCount")) == 0;
}

Delivery answer(Client& client, const Command& command, bool& invoked) {
    const auto& thread = command.target.threadId;
    if (!pending(client, thread).contains(command.questionId)) return result(Outcome::QuestionGone);
    const auto watching = wait(client, command.target, WaitFor::Run, 1);
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
        invoked = true;
        const auto responded = client.call(PendingRequest, {{"action", "respond"}, {"threadId", thread},
            {"requestId", command.questionId}, {"answers", answers}});
        position(member(responded, "sequence"));
        return result(Outcome::Accepted, {}, command.target);
    } catch (const Failure& error) {
        if (error.reason == Failure::Reason::QuestionGone) return result(Outcome::QuestionGone);
        throw;
    }
}

Json launchThread(Client& client, Json args, std::string& warning) {
    try {
        return client.call("t3_thread_launch",args);
    } catch (const Failure& failure) {
        if (failure.reason != Failure::Reason::DelegatedFromRejected || !args.erase("delegatedFrom")) throw;
        // Validation rejected the first request before dispatch. Keep the same idempotency key.
        warning = "T3 rejected the delegatedFrom launch field. Retried without linking the origin Session.";
        return client.call("t3_thread_launch",std::move(args));
    }
}

Delivery launchPinned(Client& client, const Command& command, Json args, std::string_view pin, std::string& warning) {
    auto prompt = str(args,"message");
    args.erase("message");
    const auto launched = launchThread(client,std::move(args),warning);
    const auto threadId = str(launched,"threadId");
    if (threadId.empty() || str(launched,"projectId") != command.projectId ||
        !member(launched,"runId").is_null()) invalidResponse();

    const auto until = Clock::now() + std::chrono::seconds(8);
    while (Clock::now() < until) {
        const auto head = threadHead(client,threadId);
        const auto& thread = head.at("thread");
        if (str(thread,"projectId") != command.projectId || str(thread,"createdBy") != "agent" ||
            str(thread,"creationSource") != "mcp" || boolean(thread,"archived")) invalidResponse();
        if (!member(thread,"latestRunId").is_null() || !member(thread,"activeRunId").is_null())
            return result(Outcome::Unknown,"The provisioned specialist already has a Run. Check its saved action in T3.");
        if (member(thread,"worktreePath").is_null()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        const auto workspace = workspaceHead(client,command.projectId,thread);
        if (!workspace || !workspace->clean || workspace->commit != pin)
            return {Outcome::NotAccepted,{},"The provisioned specialist worktree does not match the saved commit",true};
        const auto sent = client.call("t3_thread_send",{{"threadId",threadId},{"mode","queue"},
            {"message",std::move(prompt)},{"clientRequestId",command.actionId+":start"}});
        if (str(sent,"threadId") != threadId || str(sent,"runId").empty() ||
            (str(sent,"delivery") != "started" && str(sent,"delivery") != "queued")) invalidResponse();
        return result(Outcome::Accepted,{}, {threadId,str(sent,"runId"),str(launched,"link")});
    }
    return result(Outcome::Unknown,"The specialist worktree is still being provisioned");
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
            const bool launch = command.tool == "delegate";
            const auto pin = launch ? launchArgs(command).pin : std::string{};
            auto connection = credential(command.environmentId, command.config);
            requireOperate(connection);
            if (launch) {
                if (!pin.empty() && !loopbackGitHost(checkedUrl(connection.mcpUrl).origin))
                    return result(Outcome::Settings,"Pinned specialist worktrees require a local T3 environment");
                Client client(std::move(connection));
                checkVerificationRoute(client,command.config,command.environmentId);
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
        std::string launchWarning;
        try {
            verifyCommand(command);
            const bool launch = command.tool == "delegate";
            const bool selected = command.tool == "start_in_session";
            auto launchArguments = launch ? launchArgs(command) : LaunchArguments{};
            auto connection = credential(command.environmentId, command.config);
            if (command.authGeneration.empty() || connection.generation != command.authGeneration)
                return {Outcome::Auth, {}, "The T3 sign-in changed before Flock sent this action", true};
            requireOperate(connection);
            const auto origin = checkedUrl(connection.mcpUrl).origin;
            Client client(std::move(connection));
            if (command.tool == "answer_session_question") return answer(client, command, invoked);
            if (command.tool == "message_session") {
                const auto environment = client.call("t3_environment_read",Json::object());
                if (str(environment,"environmentId") != command.environmentId)
                    return {Outcome::Settings,{},"The OAuth endpoint belongs to a different T3 environment",true};
                invoked = true;
                const auto reply = client.call("t3_thread_send",{{"threadId",command.target.threadId},
                    {"mode","queue"},{"message",command.instruction},{"clientRequestId",command.actionId}});
                const auto run = str(reply,"runId");
                const auto delivery = str(reply,"delivery");
                if (str(reply,"threadId") != command.target.threadId || run.empty() ||
                    (delivery != "started" && delivery != "queued")) invalidResponse();
                return {Outcome::Accepted,{command.target.threadId,run,str(reply,"link")},{},false,delivery};
            }
            Json reply;
            if (launch) {
                checkVerificationRoute(client,command.config,command.environmentId);
                if (command.sessionOrigin) {
                    const auto environment = client.call("t3_environment_read",Json::object());
                    if (str(environment,"environmentId") != command.environmentId)
                        return {Outcome::Settings,{}, "The OAuth endpoint belongs to a different T3 environment",true};
                }
                if (!launchArguments.pin.empty()) {
                    if (!loopbackGitHost(origin))
                        return {Outcome::Settings,{},"Pinned specialist worktrees require a local T3 environment",true};
                    invoked = true;
                    auto delivery = launchPinned(client,command,std::move(launchArguments.parameters),launchArguments.pin,launchWarning);
                    delivery.launchWarning = std::move(launchWarning);
                    useLocalThreadLink(delivery.run,command.environmentId,origin);
                    return delivery;
                }
                invoked = true;
                reply = launchThread(client,std::move(launchArguments.parameters),launchWarning);
                if (str(reply, "projectId") != command.projectId) invalidResponse();
            } else {
                Json previous;
                if (selected) {
                    const auto environment = client.call("t3_environment_read",Json::object());
                    if (str(environment,"environmentId") != command.environmentId)
                        return {Outcome::Settings,{},"The OAuth endpoint belongs to a different T3 environment",true};
                    previous = threadHead(client,command.target.threadId).at("thread");
                    if (str(previous,"projectId") != command.projectId || boolean(previous,"archived"))
                        return {Outcome::Settings,{},"The selected T3 Session moved projects or was archived",true};
                } else if (!followUpReady(client, command)) {
                    return result(Outcome::NotAccepted, "T3 has other work or the assigned Run could not be confirmed as the latest settled Run");
                }
                invoked = true;
                reply = client.call("t3_thread_send", {{"threadId", command.target.threadId}, {"mode", "queue"},
                    {"message", sessionPrompt(command.actionId,command.instruction)}, {"clientRequestId", command.actionId}});
                const auto delivery = str(reply, "delivery");
                const auto runId = str(reply,"runId");
                if (str(reply, "threadId") != command.target.threadId || runId == command.target.runId ||
                    (delivery != "started" && delivery != "queued")) invalidResponse();
                if (selected && (str(reply,"messageId").empty() || runId == str(previous,"activeRunId") ||
                                 runId == str(previous,"latestRunId"))) invalidResponse();
            }
            RunRef run{str(reply, "threadId"), str(reply, "runId"), str(reply, "link")};
            useLocalThreadLink(run,command.environmentId,origin);
            auto delivery = hasRun(run) ? result(Outcome::Accepted,{},std::move(run))
                : result(Outcome::Unknown,"T3 has not returned the assigned Run ID yet",std::move(run));
            delivery.launchWarning = std::move(launchWarning);
            return delivery;
        } catch (const Failure& failure) {
            if (invoked && command.tool != "answer_session_question") {
                auto delivery = workReply(failure);
                delivery.launchWarning = std::move(launchWarning);
                return delivery;
            }
            auto refused = failureDelivery(failure);
            refused.definitelyUnsent = !invoked;
            return refused;
        }
    }

    Delivery reconcile(const Command& command) override {
        try {
            verifyCommand(command);
            auto connection = credential(command.environmentId, command.config);
            const auto origin = checkedUrl(connection.mcpUrl).origin;
            Client client(std::move(connection));
            if (command.tool == "delegate" || command.tool == "follow_up" || command.tool == "start_in_session") {
                checkVerificationRoute(client,command.config,command.environmentId);
                auto recovered = reconcileWork(client, command);
                if (command.tool == "delegate" && recovered.kind == Outcome::Accepted) {
                    const auto pin = launchArgs(command).pin;
                    if (!pin.empty()) {
                        if (!loopbackGitHost(origin))
                            return result(Outcome::Unknown,"The recovered specialist workspace does not match the saved commit");
                        const auto head = threadHead(client,recovered.run.threadId);
                        const auto workspace = workspaceHead(client,command.projectId,head.at("thread"));
                        if (!workspace || !workspace->clean || workspace->commit != pin)
                            return result(Outcome::Unknown,"The recovered specialist workspace does not match the saved commit");
                    }
                }
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

    std::string sessionMessageWake(const SessionTarget& target, const RunRef& run) override {
        try {
            Client client(credential(target.environmentId,Json::object()));
            const auto reply = wait(client,run,WaitFor::Run,1);
            const auto effective = str(reply,"runId");
            const auto status = str(reply,"status");
            if (boolean(reply,"timedOut") && !active(status)) return {};
            if (effective != run.runId && !isRestartContinuation(client,run,effective)) return {};
            if (terminal(status)) {
                return status == "completed" ? "finished" : "failed";
            }
            if (!active(status)) return "unknown";
            if (status == "queued" || status == "preparing" || status == "starting") return {};
            if (pending(client,run.threadId).empty()) return {};
            // The thread-level question list may have changed while the exact Run ended.
            const auto head = threadHead(client,run.threadId).at("thread");
            if (str(head,"activeRunId") == effective) return "waiting_for_person";
            return {};
        } catch (const Failure& failure) {
            return failure.reason == Failure::Reason::HistoryNotFound ||
                   failure.reason == Failure::Reason::CredentialExpired ? "unknown" : "";
        }
    }

    Observation observe(const std::string& environmentId, const Json& config, const RunRef& run) override {
        if (!hasRun(run)) return state(Observed::Unavailable, "The assigned T3 Run is missing");
        try {
            auto connection = credential(environmentId,config);
            const auto origin = checkedUrl(connection.mcpUrl).origin;
            Client client(std::move(connection));
            checkVerificationRoute(client,config,environmentId,run.threadId);
            const auto reply = wait(client, run, WaitFor::DelegatedWork);
            const auto effective = str(reply, "runId");
            const auto verification = config.find("commitVerification");
            if (verification != config.end() && effective != run.runId && !isRestartContinuation(client,run,effective))
                return state(Observed::Unavailable,"Could not identify the Run's restart continuation");
            const auto status = str(reply, "status");
            const bool finished = terminal(status);
            if (status == "queued") return state(Observed::Running,status);
            const auto timedOut = boolean(reply,"timedOut");
            std::optional<Json> head;
            if (timedOut && status == "cancelled" && effective == run.runId)
                head = threadHead(client,run.threadId,50);
            if (finished && (!timedOut || (head && confirmsQueueCancellation(client,run,*head)))) {
                auto source = RunRef{run.threadId, effective, run.link};
                auto observed = state(Observed::Finished, status);
                const auto later = reply.value("resultRun", Json::object());
                const auto laterId = str(later, "runId");
                if (!timedOut && (laterId.empty() || !terminal(str(later,"status")))) invalidResponse();
                if (!timedOut && ownsResult(client, run, effective, laterId)) {
                    source.runId = laterId;
                    observed.status = str(later, "status");
                }
                auto problem = pinnedEvidenceProblem(client,config,source,observed.status,origin);
                if (problem && problem->availability == RunEvidence::Availability::ReportUnavailable)
                    return state(Observed::Unavailable,std::move(problem->detail));
                if (problem) observed.status = "failed";
                auto evidence = problem ? std::move(*problem) : readTerminalEvidence(client,source,observed.status);
                if (verification != config.end()) {
                    try {
                        observed.checkedCommit = checkedCommit(client,run,{run.threadId,effective,run.link},source,
                                                               evidence,str(*verification,"expectedCommit"));
                    } catch (const Failure&) {
                        // A completed Run can be read again if its evidence has not arrived yet.
                    }
                    observed.commitEvidenceUnavailable = observed.status == "completed" && observed.checkedCommit.empty();
                }
                observed.evidence = evidence.availability;
                observed.report = std::move(evidence.report);
                observed.itemId = std::move(evidence.itemId);
                observed.detail = std::move(evidence.detail);
                observed.truncated = evidence.truncated;
                observed.resultRunId = source.runId;
                observed.sessionCreatedAt = std::move(evidence.sessionCreatedAt);
                return observed;
            }
            if (!finished && !active(status)) invalidResponse();
            if (!head) head = threadHead(client,run.threadId);
            const auto& thread = head->at("thread");
            const auto count = position(member(thread, "pendingRequestCount"));
            if (count == 0) return state(finished ? Observed::Settling : Observed::Running, status);
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

    RunEvidence readRunEvidence(const std::string& environmentId, const Json& config, const RunRef& source,
                                std::string_view status) override {
        try {
            auto connection = credential(environmentId,config);
            const auto origin = checkedUrl(connection.mcpUrl).origin;
            Client client(std::move(connection));
            if (auto problem = pinnedEvidenceProblem(client,config,source,status,origin)) return std::move(*problem);
            return readTerminalEvidence(client,source,status);
        } catch (const Failure& failure) {
            return unavailableReport(failure);
        }
    }

    InterruptOutcome interrupt(const std::string& environmentId, const Json& config, const RunRef& run) override {
        if (!hasRun(run)) throw Failure(Failure::Kind::Settings, "The assigned T3 Run is missing");
        auto connection = credential(environmentId, config);
        requireOperate(connection);
        Client client(std::move(connection));
        checkVerificationRoute(client,config,environmentId,run.threadId);
        // An explicit wait can resolve T3's own restart continuation of the assigned Run.
        const auto current = wait(client, run, WaitFor::Run, 1);
        const auto effective = str(current, "runId");
        if (config.contains("commitVerification") && effective != run.runId && !isRestartContinuation(client,run,effective))
            throw Failure(Failure::Kind::Wire,"Could not identify the Run's restart continuation");
        const auto status = str(current,"status");
        if (terminal(status)) return InterruptOutcome::AlreadyTerminal;
        if (status == "queued" && effective == run.runId) {
            client.call("t3_queue",{{"action","cancel"},{"threadId",run.threadId},{"queuedRunId",run.runId}});
            return InterruptOutcome::Requested;
        }
        if (!active(status)) invalidResponse();
        const auto reply = client.call("t3_thread_interrupt", {{"threadId", run.threadId}, {"runId", effective}});
        const auto replyStatus = str(reply, "status");
        if (str(reply, "threadId") != run.threadId) invalidResponse();
        if (replyStatus == "no_active_run") return InterruptOutcome::NoActiveRun;
        if (str(reply, "runId") != effective) invalidResponse();
        if (replyStatus == "interrupt_requested") return InterruptOutcome::Requested;
        if (terminal(replyStatus)) return InterruptOutcome::AlreadyTerminal;
        invalidResponse();
    }

    QueueCancelOutcome cancelQueuedSessionMessage(const SessionTarget& target, std::string_view run) override {
        using Outcome = QueueCancelOutcome;
        try {
            auto connection = credential(target.environmentId,Json::object());
            requireOperate(connection);
            Client client(std::move(connection));
            const auto environment = client.call("t3_environment_read",Json::object());
            if (str(environment,"environmentId") != target.environmentId) return Outcome::Retry;
            try {
                const auto reply = client.call("t3_queue",{{"action","cancel"},{"threadId",target.threadId},{"queuedRunId",run}});
                position(member(reply,"sequence"));
                return Outcome::Cancelled;
            } catch (const Failure& failure) {
                if (failure.kind != Failure::Kind::Tool) return Outcome::Retry;
            }

            // T3 rejects the mutation if the Run left the queue. Read the exact Run to
            // distinguish that race from a transient failure or a retried cancellation.
            const auto head = threadHead(client,target.threadId,50);
            for (const auto& entry : array(head,"recentRuns")) {
                if (str(entry,"runId") != run) continue;
                const auto status = str(entry,"status");
                const auto started = entry.find("startedAt");
                if (started != entry.end() && started->is_string() && !started->get_ref<const std::string&>().empty())
                    return Outcome::AlreadyStarted;
                if (status == "cancelled" && started != entry.end() && started->is_null()) return Outcome::Cancelled;
                break;
            }
            const auto exact = wait(client,{target.threadId,std::string(run),{}},WaitFor::Run,1);
            if (str(exact,"runId") != run) return Outcome::NotCancellable;
            const auto status = str(exact,"status");
            if (status == "running" || status == "waiting" || status == "completed") return Outcome::AlreadyStarted;
            if (terminal(status)) return Outcome::NotCancellable;
        } catch (const Failure& failure) {
            if (failure.reason == Failure::Reason::HistoryNotFound) return Outcome::NotCancellable;
        }
        return Outcome::Retry;
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

    std::pair<std::vector<std::string>,std::optional<Id>> projectSessions(
        std::string_view environment, std::string_view project, Id offset, SessionReadDeadline deadline) override {
        std::vector<std::string> threads;
        try {
            Client client(loadCredential(directory_,std::string(environment)),deadline);
            for (int pageIndex = 0; pageIndex < 20; ++pageIndex) {
                Json args{{"includeSubagents",false},{"limit",50},{"cursor",offset}};
                if (project.empty()) args["allProjects"] = true;
                else args["projectId"] = project;
                const auto page = client.call("t3_thread_list",std::move(args));
                if (project.empty() ? !member(page,"projectId").is_null() : str(page,"projectId") != project)
                    invalidResponse();
                const auto& listed = array(page,"threads");
                for (const auto& thread : listed) {
                    const auto id = str(thread,"threadId");
                    const auto threadProject = str(thread,"projectId");
                    if (id.empty() || threadProject.empty() || (!project.empty() && threadProject != project))
                        invalidResponse();
                    threads.push_back(id);
                }
                const auto next = cursor(page,"nextCursor");
                if (!next) return {std::move(threads),std::nullopt};
                if (*next <= offset || *next != offset + static_cast<Position>(listed.size())) invalidResponse();
                offset = *next;
            }
            return {std::move(threads),offset};
        } catch (const SessionReadTimeout&) {
            return {std::move(threads),offset};
        } catch (const Failure& error) {
            if (error.kind == Failure::Kind::Auth || error.kind == Failure::Kind::Settings) return {};
            throw;
        }
    }

    std::vector<T3Project> projects(std::string_view environment) override {
        Client client(loadCredential(directory_,std::string(environment)),
                      std::chrono::steady_clock::now()+std::chrono::seconds(5));
        std::vector<T3Project> projects;
        std::set<std::string> seen;
        Id cursor = 0;
        for (;;) {
            const auto page = client.call("t3_project_list",{{"cursor",cursor},{"limit",100}});
            for (const auto& project : array(page,"projects")) {
                auto id = str(project,"id");
                auto title = str(project,"title");
                if (id.empty() || id.size() > 1024 || title.empty() || !seen.insert(id).second) invalidResponse();
                const auto archived = project.find("archivedAt");
                if (archived != project.end() && !archived->is_null()) {
                    if (!archived->is_string()) invalidResponse();
                    continue;
                }
                auto root = str(project,"workspaceRoot");
                if (root.empty()) invalidResponse();
                if (const auto end = root.find_last_not_of("/\\"); end != root.npos) {
                    root.resize(end+1);
                    if (const auto separator = root.find_last_of("/\\"); separator != root.npos)
                        root.erase(0,separator+1);
                }
                projects.push_back({std::move(id),std::move(title),std::move(root)});
            }
            const auto& next = member(page,"nextCursor");
            if (next.is_null()) return projects;
            const auto offset = position(next);
            if (offset <= cursor) invalidResponse();
            cursor = offset;
        }
    }

    std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool includeMessages,
                                                  SessionReadDeadline deadline) override {
        return t3detail::sessionMetadata(directory_,target,includeMessages,deadline);
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
        if (!config.is_object())
            throw Failure(Failure::Kind::Settings, "Invalid T3 Bot settings");
        return loadCredential(directory_, environment);
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
