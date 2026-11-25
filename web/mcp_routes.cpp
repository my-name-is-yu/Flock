#include "mcp_routes.hpp"
#include "views.hpp"
#include "flock/external_mcp.hpp"
#include "flock/participation.hpp"
#include "flock/skill_export.hpp"

#include <algorithm>
#include <limits>

namespace flockv0::web {
namespace {
constexpr std::size_t maxPayload = 64 * 1024;
constexpr std::size_t maxRequestText = 8192;
constexpr std::string_view askTool = "flock_ask";
constexpr std::string_view listTool = "flock_bots";
constexpr std::string_view stopTool = "flock_stop";
constexpr std::string_view submitTool = "flock_submit_task";
constexpr std::string_view statusTool = "flock_get_task";
constexpr std::string_view answerTool = "flock_answer_question";
constexpr std::string_view mcpProtocol = "2025-11-25";
constexpr std::size_t maxAnswerText = 8000;

struct InvalidToolArguments {
    std::string message = "Invalid tool arguments";
};

std::string utf8Limit(std::size_t maxBytes) {
    return "Maximum " + std::to_string(maxBytes) + " UTF-8 bytes.";
}

void reply(httplib::Response& res, int status, const Json& data) {
    res.status = status;
    res.set_content(data.dump(), "application/json; charset=utf-8");
}

std::string identifier(const Json& object, std::string_view field, std::size_t maxBytes = 1024) {
    const auto value = object.find(field);
    if (value == object.end() || !value->is_string()) throw InvalidToolArguments{};
    const auto& text = value->get_ref<const std::string&>();
    if (text.empty() || text.size() > maxBytes || text.front() == ' ' || text.back() == ' ' ||
        std::ranges::any_of(text, [](unsigned char c) { return c < 32 || c == 127; }))
        throw InvalidToolArguments{};
    return text;
}

std::string requestText(const Json& object, std::string_view field, std::size_t maxBytes = maxRequestText) {
    const auto value = object.find(field);
    if (value == object.end() || !value->is_string()) throw InvalidToolArguments{};
    const auto& text = value->get_ref<const std::string&>();
    if (text.size() > maxBytes) throw InvalidToolArguments{std::string(field) + ": " + utf8Limit(maxBytes)};
    if (text.empty() || text.find_first_not_of(" \t\r\n") == text.npos || text.find('\0') != text.npos) throw InvalidToolArguments{};
    return text;
}

Id taskInteger(const Json& arguments, std::string_view name, Id minimum) {
    const auto value = arguments.find(name);
    if (value == arguments.end() || !value->is_number_integer() ||
        (value->is_number_unsigned() && value->get<std::uint64_t>() > std::numeric_limits<Id>::max()) || value->get<Id>() < minimum)
        throw InvalidToolArguments{};
    return value->get<Id>();
}

void rpcError(httplib::Response& res, int status, const Json& id, int code, std::string_view message) {
    reply(res, status, {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}

void rpcResult(httplib::Response& res, const Json& id, Json result) {
    reply(res, 200, {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

Json toolContent(std::string_view text) {
    return Json::array({{{"type", "text"}, {"text", text}}});
}

Json structuredResult(const Json& data) {
    return {{"structuredContent", data}, {"content", toolContent(data.dump())}};
}

Json byteLimitedString(std::size_t maxBytes) {
    return {{"type", "string"}, {"minLength", 1}, {"description", utf8Limit(maxBytes)}};
}

Json sessionToolDefinitions() {
    const Json bot{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}, {"pattern", "^[A-Za-z0-9][A-Za-z0-9._-]*$"}};
    const auto thread = byteLimitedString(256);
    return Json::array(
        {{{"name", listTool},
          {"description", "List the Flock Bots you can ask. Use a Bot's id as bot in flock_ask or flock_stop."},
          {"annotations", {{"readOnlyHint", true}}},
          {"inputSchema", {{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}}}},
         {{"name", askTool},
          {"description", "Ask a Flock Bot from this T3 Session. Use currentThreadId from t3_thread_list. "
                          "Bot messages arrive in this same Session prefixed with the Bot's name. No polling is needed. "
                          "Show messages addressed to the person, such as those marked \"For the person:\", and let them answer. "
                          "Forward their reply with flock_ask using the same bot and threadId and a new requestId. "
                          "Retry a request with its original requestId and identical arguments."},
          {"annotations", {{"idempotentHint", true}}},
          {"inputSchema",
           {{"type", "object"},
            {"additionalProperties", false},
            {"required", {"bot", "text", "threadId", "requestId"}},
            {"properties",
             {{"bot", bot},
              {"threadId", thread},
              {"requestId", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}, {"pattern", "^[A-Za-z0-9._:-]+$"}}},
              {"text", byteLimitedString(maxRequestText)}}}}}},
         {{"name", stopTool},
          {"description", "Remove this Bot and its invitees from the T3 Session. Cancel their queued messages "
                          "and report any that already started. Stop child Sessions they launched from here. The person's work continues. "
                          "A new flock_ask to a removed Bot brings it back."},
          {"annotations", {{"idempotentHint", true}}},
          {"inputSchema",
           {{"type", "object"},
            {"additionalProperties", false},
            {"required", {"bot", "threadId"}},
            {"properties", {{"bot", bot}, {"threadId", thread}}}}}}});
}

Json externalToolDefinitions() {
    const Json id{{"type", "integer"}, {"minimum", 1}};
    const Json version{{"type", "integer"}, {"minimum", 0}};
    const auto schema = [](Json properties, Json required) -> Json {
        return {
            {"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}, {"required", std::move(required)}};
    };
    return Json::array(
        {{{"name", submitTool},
          {"description", "Send a task to an authorized Flock Bot using its Bot ID. Save the taskId to check progress. "
                          "On a lost response, retry with the same requestId, bot and text."},
          {"annotations", {{"idempotentHint", true}}},
          {"inputSchema",
           schema({{"bot", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}, {"pattern", "^[A-Za-z0-9][A-Za-z0-9._-]*$"}}},
                   {"text", byteLimitedString(maxRequestText)},
                   {"requestId", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}, {"pattern", "^[A-Za-z0-9._:-]+$"}}}},
                  {"bot", "text", "requestId"})}},
         {{"name", statusTool},
          {"description", "Check one of your tasks for its current status, pending question, final result and Session link."},
          {"annotations", {{"readOnlyHint", true}}},
          {"inputSchema", schema({{"taskId", id}}, {"taskId"})}},
         {{"name", answerTool},
          {"description", "Answer the pending question from flock_get_task. Copy its waitId, targetRevision and "
                          "targetGeneration. Refresh the task if the question changed or has already been answered."},
          {"inputSchema", schema({{"taskId", id},
                                  {"waitId", id},
                                  {"targetRevision", version},
                                  {"targetGeneration", version},
                                  {"text", byteLimitedString(maxAnswerText)}},
                                 {"taskId", "waitId", "targetRevision", "targetGeneration", "text"})}}});
}

Json callExternalTool(const WebCallbacks& callbacks, ExternalMcp& external, const ExternalMcpConnection& connection, const Json& tool,
                      const Json& arguments) {
    if (tool == submitTool) {
        if (arguments.size() != 3) throw InvalidToolArguments{};
        const auto requestId = identifier(arguments, "requestId", 128);
        const auto task =
            external.submit(connection, identifier(arguments, "bot", 128), requestId, requestText(arguments, "text"), callbacks.intake);
        return structuredResult({{"taskId", task}, {"requestId", requestId}, {"status", "received"}});
    }
    if (tool == statusTool) {
        if (arguments.size() != 1) throw InvalidToolArguments{};
        auto data = external.status(connection, taskInteger(arguments, "taskId", 1));
        const auto link = externalSessionLink(data.at("sessionRun"));
        data.erase("sessionRun");
        data["sessionLink"] = link.empty() ? Json(nullptr) : Json(link);
        return structuredResult(data);
    }
    if (tool == answerTool) {
        if (arguments.size() != 5) throw InvalidToolArguments{};
        external.answer(connection, taskInteger(arguments, "taskId", 1), taskInteger(arguments, "waitId", 1),
                        taskInteger(arguments, "targetRevision", 0), taskInteger(arguments, "targetGeneration", 0),
                        requestText(arguments, "text", maxAnswerText));
        return structuredResult({{"accepted", true}});
    }
    return nullptr;
}

bool containsBot(const Json& bots, std::string_view id) {
    return std::ranges::any_of(bots, [&](const Json& bot) { return bot.value("id", Json(nullptr)) == id; });
}

Json callSessionTool(Store& store, const WebCallbacks& callbacks, const T3Connection& connection, const Json& tool, const Json& arguments) {
    if (tool != listTool && tool != askTool && tool != stopTool) return nullptr;
    SessionParticipation participation(store, *callbacks.sessionT3);
    if (tool == listTool) {
        if (!arguments.empty()) throw InvalidToolArguments{};
        const auto actor = participation.person(connection);
        const auto bots = callbacks.bots();
        const auto names = flockSkillNames(bots);
        auto rows = Json::array();
        for (const auto& bot : bots) {
            const auto botId = bot.value("id", std::string{});
            if (!names.contains(botId) || !store.policy().allows(actor, Action::bot_decide, botId)) continue;
            try {
                if (callbacks.intake(botId, "").botId != botId) continue;
            } catch (const std::exception&) {
                continue;
            }
            rows.push_back({{"id", botId},
                            {"name", bot.value("name", botId)},
                            {"role", bot.value("role", std::string("custom"))},
                            {"skill", names.at(botId)}});
        }
        return structuredResult({{"bots", std::move(rows)}});
    }
    if (arguments.size() != (tool == askTool ? 4u : 2u)) throw InvalidToolArguments{};
    const auto bot = identifier(arguments, "bot", 128);
    const auto thread = identifier(arguments, "threadId", 256);
    if (tool == stopTool) {
        const auto stopped = participation.stop(connection, bot, thread);
        return {{"structuredContent", stopped.data()}, {"content", toolContent(stopped.summary())}};
    }
    const auto requestId = identifier(arguments, "requestId", 128);
    const SessionAsk request{bot, requestText(arguments, "text"), thread, requestId};
    const auto messageId = participation.ask(connection, request, [&](std::string_view botId, std::string_view text) {
        if (!containsBot(callbacks.bots(), botId)) throw Conflict("The Bot is unavailable");
        try {
            return callbacks.intake(botId, text);
        } catch (const std::exception&) {
            throw Conflict("The Bot is unavailable");
        }
    });
    return {{"structuredContent", {{"messageId", messageId}, {"requestId", requestId}, {"status", "received"}}},
            {"content", toolContent("Request accepted. The Bot's messages arrive here with its name. No polling is needed.")}};
}

bool supportedProtocol(std::string_view value) {
    return value == mcpProtocol || value == "2025-06-18";
}

std::string bearerToken(const httplib::Request& req) {
    if (req.get_header_value_count("Authorization") != 1) return {};
    const auto authorization = req.get_header_value("Authorization");
    return authorization.starts_with("Bearer ") ? authorization.substr(7) : std::string{};
}

void requireBearer(httplib::Response& res) {
    res.set_header("WWW-Authenticate", "Bearer realm=\"Flock\"");
    rpcError(res, 401, nullptr, -32001, "A valid Flock connection bearer token is required");
}

bool acceptMcpPost(const httplib::Request& req, httplib::Response& res) {
    if (req.method != "POST") {
        res.status = 405;
        res.set_header("Allow", "POST");
        return false;
    }
    const auto reject = [&](int status, std::string_view message) {
        rpcError(res, status, nullptr, -32600, message);
        return false;
    };
    if (!req.params.empty()) return reject(400, "Unexpected URL parameters");
    if (req.body.size() > maxPayload) return reject(413, "Request too large");
    const auto mediaType = req.get_header_value("Content-Type");
    if (req.get_header_value_count("Content-Type") != 1 || mediaType.substr(0, mediaType.find(';')) != "application/json")
        return reject(415, "Expected application/json");
    if (req.get_header_value_count("MCP-Protocol-Version") > 1 ||
        (req.has_header("MCP-Protocol-Version") && !supportedProtocol(req.get_header_value("MCP-Protocol-Version"))))
        return reject(400, "Unsupported MCP protocol version");
    return true;
}

void mcpRequest(const httplib::Request& req, httplib::Response& res, Json (*toolDefinitions)(), const auto& callTool) {
    const auto message = Json::parse(req.body, nullptr, false);
    if (message.is_discarded()) return rpcError(res, 400, nullptr, -32700, "Invalid JSON");
    if (!message.is_object() || message.value("jsonrpc", Json(nullptr)) != "2.0" || !message.contains("method") ||
        !message.at("method").is_string())
        return rpcError(res, 400, nullptr, -32600, "Invalid JSON-RPC request");
    const auto& method = message.at("method").get_ref<const std::string&>();
    const auto id = message.find("id");
    if (id == message.end()) {
        // Notifications carry no response.
        res.status = 202;
        return;
    }
    if (!id->is_string() && !id->is_number_integer()) return rpcError(res, 400, nullptr, -32600, "Invalid request ID");
    const auto params = message.value("params", Json::object());
    if (!params.is_object()) return rpcError(res, 200, *id, -32602, "Invalid parameters");

    if (method == "initialize") {
        const auto protocol = params.find("protocolVersion");
        if (protocol == params.end() || !protocol->is_string()) return rpcError(res, 200, *id, -32602, "A protocol version is required");
        const auto requested = protocol->get<std::string>();
        return rpcResult(res, *id,
                         {{"protocolVersion", supportedProtocol(requested) ? std::string_view(requested) : mcpProtocol},
                          {"serverInfo", {{"name", "flock"}, {"version", "1.0.0"}}},
                          {"capabilities", {{"tools", Json::object()}}}});
    }
    if (method == "ping") return rpcResult(res, *id, Json::object());
    if (method == "tools/list") return rpcResult(res, *id, {{"tools", toolDefinitions()}});
    if (method != "tools/call") return rpcError(res, 200, *id, -32601, "Method not found");
    const auto tool = params.value("name", Json(nullptr));
    const auto arguments = params.value("arguments", Json::object());
    try {
        if (!arguments.is_object()) throw InvalidToolArguments{};
        auto result = callTool(tool, arguments);
        if (result.is_null()) return rpcError(res, 200, *id, -32602, "Unknown tool");
        return rpcResult(res, *id, std::move(result));
    } catch (const InvalidToolArguments& error) {
        rpcError(res, 200, *id, -32602, error.message);
    } catch (const Conflict& error) {
        rpcResult(res, *id, {{"isError", true}, {"content", toolContent(error.what())}});
    } catch (const std::exception&) {
        rpcError(res, 200, *id, -32603, "Flock could not process the request");
    }
}

void registerMcp(httplib::Server& server, const char* path, const httplib::Server::Handler& handler) {
    server.Post(path, handler);
    server.Get(path, handler);
    server.Delete(path, handler);
}
}

void mcpRoutes(httplib::Server& server, Store& store, const WebCallbacks& callbacks) {
    registerMcp(server, sessionMcpPath, [&store, &callbacks](const httplib::Request& req, httplib::Response& res) {
        const auto connection = T3Connections(store).authenticate(bearerToken(req));
        if (!connection) return requireBearer(res);
        if (!acceptMcpPost(req, res)) return;
        if (!callbacks.sessionT3) return rpcError(res, 503, nullptr, -32603, "Flock Session tools are unavailable");
        mcpRequest(req, res, sessionToolDefinitions,
                   [&](const Json& tool, const Json& args) { return callSessionTool(store, callbacks, *connection, tool, args); });
    });
    registerMcp(server, externalMcpPath, [&store, &callbacks](const httplib::Request& req, httplib::Response& res) {
        ExternalMcp external(store);
        const auto connection = external.authenticate(bearerToken(req));
        if (!connection) return requireBearer(res);
        if (!acceptMcpPost(req, res)) return;
        mcpRequest(req, res, externalToolDefinitions,
                   [&](const Json& tool, const Json& args) { return callExternalTool(callbacks, external, *connection, tool, args); });
    });
}

}
