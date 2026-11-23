#include "session_handoff_routes.hpp"
#include "access_routes.hpp"
#include "flock/skill_export.hpp"

#include <algorithm>
#include <charconv>

namespace flockv0::web {
namespace {
constexpr std::size_t maxPayload = 64 * 1024;
constexpr std::size_t maxRequestText = 8192;
constexpr std::string_view invokeTool = "invoke_flock_bot";
constexpr std::string_view listTool = "list_flock_bots";
constexpr std::string_view mcpProtocol = "2025-11-25";

struct InvalidHandoffInput {};

void reply(httplib::Response& res, int status, const Json& data) {
    res.status = status;
    res.set_content(data.dump(),"application/json; charset=utf-8");
}

std::string identifier(const Json& object, std::string_view field, std::size_t maxBytes = 1024) {
    const auto value = object.find(field);
    if (value == object.end() || !value->is_string()) throw InvalidHandoffInput{};
    const auto& text = value->get_ref<const std::string&>();
    if (text.empty() || text.size() > maxBytes || text.front() == ' ' || text.back() == ' ' ||
        std::ranges::any_of(text,[](unsigned char c) { return c < 32 || c == 127; })) throw InvalidHandoffInput{};
    return text;
}

std::string requestText(const Json& object, std::string_view field, std::size_t maxBytes = maxRequestText) {
    const auto value = object.find(field);
    if (value == object.end() || !value->is_string()) throw InvalidHandoffInput{};
    const auto& text = value->get_ref<const std::string&>();
    if (text.empty() || text.size() > maxBytes ||
        text.find_first_not_of(" \t\r\n") == text.npos || text.find('\0') != text.npos)
        throw InvalidHandoffInput{};
    return text;
}

Json objectBody(const httplib::Request& req, std::size_t count) {
    auto body = Json::parse(req.body,nullptr,false);
    if (!body.is_object() || body.size() != count) throw InvalidHandoffInput{};
    return body;
}

Id channelId(const httplib::Request& req) {
    const auto text = req.matches[1].str();
    Id id = 0;
    const auto parsed = std::from_chars(text.data(),text.data() + text.size(),id);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || id <= 0) throw InvalidHandoffInput{};
    return id;
}

Json targetView(const ThreadTarget& target) {
    return {{"environmentId",target.environmentId},{"projectId",target.projectId},{"threadId",target.threadId}};
}

Json deliveryView(const RootDeliveryView& delivery) {
    Json view{{"commandId",delivery.commandId},{"state",rootDeliveryStateName(delivery.state)},{"receipt",nullptr},
              {"observedRunId",delivery.observedRunId.empty() ? Json(nullptr) : Json(delivery.observedRunId)}};
    if (const auto& receipt = delivery.receipt)
        view["receipt"] = {{"messageId",receipt->messageId},{"runId",receipt->runId},{"mode",receipt->mode}};
    return view;
}

Json handoffView(const SessionHandoffView& handoff) {
    const auto& invitation = handoff.origin.invitation;
    auto deliveries = Json::array();
    for (const auto& delivery : handoff.deliveries) deliveries.push_back(deliveryView(delivery));
    return {{"channelId",handoff.channel},{"target",targetView(invitation.target)},{"requestId",invitation.requestId},
            {"botId",invitation.botId},{"mode",handoff.coordination ? "coordinate" : "consult"},
            {"person",handoff.origin.personId},{"state",handoff.state},{"frozen",handoff.frozen},
            {"goal",handoff.permission ? Json(handoff.permission->goal) : Json(nullptr)},
            {"allowDelegation",handoff.permission && handoff.permission->allowDelegation},
            {"generation",handoff.generation},{"residualCommandIds",handoff.residualCommandIds},
            {"deliveries",std::move(deliveries)},{"eventCursor",handoff.eventCursor},
            {"latestDecision",handoff.latestDecision},{"pendingQuestion",handoff.pendingQuestion},
            {"work",handoff.work},{"admittedChildIds",handoff.admittedChildIds}};
}

template<class Action>
void withHandoff(const WebCallbacks& callbacks, const httplib::Request& req, httplib::Response& res, Action action) {
    const auto actor = person(callbacks,req);
    if (actor.id.empty()) return reply(res,401,{{"error","identity_required"}});
    if (!callbacks.sessionHandoffs) return reply(res,503,{{"error","handoff_unavailable"}});
    if (req.body.size() > maxPayload) return reply(res,413,{{"error","payload_too_large"}});
    if (!req.params.empty()) return reply(res,400,{{"error","invalid_input"}});
    try {
        action(*callbacks.sessionHandoffs,actor);
    } catch (const InvalidHandoffInput&) {
        reply(res,400,{{"error","invalid_input"}});
    } catch (const Conflict&) {
        reply(res,403,{{"error","handoff_unavailable"}});
    } catch (const std::exception&) {
        reply(res,503,{{"error","handoff_unavailable"}});
    }
}

void rpcError(httplib::Response& res, int status, const Json& id, int code, std::string_view message) {
    reply(res,status,{{"jsonrpc","2.0"},{"id",id},{"error",{{"code",code},{"message",message}}}});
}

void rpcResult(httplib::Response& res, const Json& id, Json result) {
    reply(res,200,{{"jsonrpc","2.0"},{"id",id},{"result",std::move(result)}});
}

Json toolContent(std::string_view text) {
    return Json::array({{{"type","text"},{"text",text}}});
}

Json toolDefinitions() {
    const Json text{{"type","string"},{"minLength",1},{"maxLength",maxRequestText}};
    return Json::array({
        {{"name",listTool},{"description","List the Flock Bots you can invoke."},
         {"annotations",{{"readOnlyHint",true}}},
         {"inputSchema",{{"type","object"},{"additionalProperties",false},{"properties",Json::object()}}}},
        {{"name",invokeTool},
         {"description","Get threadId from currentThreadId in t3_thread_list. Use a fresh random requestId per logical request and retry with identical arguments. Use consult for one reply (default) or coordinate with a goal for ongoing work. Flock delivers replies to this same Thread. No polling is needed."},
         {"inputSchema",{{"type","object"},{"additionalProperties",false},
             {"required",{"threadId","botId","requestId","text"}},
             {"properties",{
                 {"threadId",{{"type","string"},{"minLength",1},{"maxLength",1024}}},
                 {"botId",{{"type","string"},{"minLength",1},{"maxLength",128}}},
                 {"requestId",{{"type","string"},{"minLength",1},{"maxLength",128},{"pattern","^[A-Za-z0-9._:-]+$"}}},
                 {"text",text},{"mode",{{"type","string"},{"enum",{"consult","coordinate"}},{"default","consult"}}},
                 {"goal",text}
             }}
         }}}
    });
}

bool containsBot(const Json& bots, std::string_view id) {
    return std::ranges::any_of(bots,[&](const Json& bot) { return bot.value("id",Json(nullptr)) == id; });
}

bool supportedProtocol(std::string_view value) {
    return value == mcpProtocol || value == "2025-06-18";
}

void mcpRequest(Store& store, const WebCallbacks& callbacks, const httplib::Request& req, httplib::Response& res) {
    const auto authorization = req.get_header_value("Authorization");
    const auto connection = req.get_header_value_count("Authorization") == 1 && authorization.starts_with("Bearer ")
        ? T3Connections(store).authenticate(std::string_view(authorization).substr(7)) : std::nullopt;
    if (!connection) {
        res.set_header("WWW-Authenticate","Bearer realm=\"Flock\"");
        return rpcError(res,401,nullptr,-32001,"A valid Flock connection bearer token is required");
    }
    if (req.method != "POST") {
        res.status = 405;
        res.set_header("Allow","POST");
        return;
    }
    if (!req.params.empty()) return rpcError(res,400,nullptr,-32600,"Unexpected URL parameters");
    if (req.body.size() > maxPayload) return rpcError(res,413,nullptr,-32600,"Request too large");
    const auto mediaType = req.get_header_value("Content-Type");
    if (req.get_header_value_count("Content-Type") != 1 || mediaType.substr(0,mediaType.find(';')) != "application/json")
        return rpcError(res,415,nullptr,-32600,"Expected application/json");
    if (req.get_header_value_count("MCP-Protocol-Version") > 1 ||
        (req.has_header("MCP-Protocol-Version") && !supportedProtocol(req.get_header_value("MCP-Protocol-Version"))))
        return rpcError(res,400,nullptr,-32600,"Unsupported MCP protocol version");
    if (!callbacks.sessionHandoffs) return rpcError(res,503,nullptr,-32603,"Flock handoffs are unavailable");

    const auto message = Json::parse(req.body,nullptr,false);
    if (message.is_discarded()) return rpcError(res,400,nullptr,-32700,"Invalid JSON");
    if (!message.is_object() || message.value("jsonrpc",Json(nullptr)) != "2.0" ||
        !message.contains("method") || !message.at("method").is_string())
        return rpcError(res,400,nullptr,-32600,"Invalid JSON-RPC request");
    const auto& method = message.at("method").get_ref<const std::string&>();
    const auto id = message.find("id");
    if (id == message.end()) {
        // Notifications carry no response and never call a tool.
        res.status = 202;
        return;
    }
    if (!id->is_string() && !id->is_number_integer()) return rpcError(res,400,nullptr,-32600,"Invalid request ID");
    const auto params = message.value("params",Json::object());
    if (!params.is_object()) return rpcError(res,200,*id,-32602,"Invalid parameters");

    if (method == "initialize") {
        const auto protocol = params.find("protocolVersion");
        if (protocol == params.end() || !protocol->is_string())
            return rpcError(res,200,*id,-32602,"A protocol version is required");
        const auto requested = protocol->get<std::string>();
        return rpcResult(res,*id,{{"protocolVersion",supportedProtocol(requested) ? std::string_view(requested) : mcpProtocol},
                                 {"serverInfo",{{"name","flock"},{"version","1.0.0"}}},
                                 {"capabilities",{{"tools",Json::object()}}}});
    }
    if (method == "ping") return rpcResult(res,*id,Json::object());
    if (method == "tools/list") return rpcResult(res,*id,{{"tools",toolDefinitions()}});
    if (method != "tools/call") return rpcError(res,200,*id,-32601,"Method not found");
    const auto tool = params.value("name",Json(nullptr));
    const auto arguments = params.value("arguments",Json::object());
    if (tool != listTool && tool != invokeTool) return rpcError(res,200,*id,-32602,"Unknown tool");

    try {
        auto& handoff = *callbacks.sessionHandoffs;
        if (tool == listTool) {
            if (!arguments.is_object() || !arguments.empty()) throw InvalidHandoffInput{};
            const auto actor = handoff.person(*connection);
            if (!actor) throw Conflict("Flock person is unavailable");
            const auto bots = callbacks.bots();
            const auto names = flockSkillNames(bots);
            auto rows = Json::array();
            for (const auto& bot : bots) {
                const auto botId = bot.value("id",std::string{});
                if (!store.policy().allows(*actor,Action::bot_decide,botId)) continue;
                try {
                    if (callbacks.intake(botId,"").botId != botId) continue;
                } catch (const Conflict&) {
                    continue;
                }
