#include "session_handoff_routes.hpp"
#include "access_routes.hpp"

#include <algorithm>
#include <charconv>

namespace flockv0::web {
namespace {
constexpr std::size_t maxPayload = 64 * 1024;
constexpr std::size_t maxRequestText = 8192;
constexpr std::size_t maxPairingCode = 128;
constexpr std::size_t maxRequestId = 128;
constexpr std::string_view requestTool = "request_flock_bot";
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
    if (!callbacks.authenticate) return reply(res,401,{{"error","identity_required"}});
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

Json toolDefinition() {
    return {{"name",requestTool},
            {"description","Submit one request using a code the person created in Flock. Flock delivers the result to the paired T3 Thread. No polling is needed."},
            {"inputSchema",{
                {"type","object"},{"additionalProperties",false},
                {"required",{"pairingCode","requestId","text"}},
                {"properties",{
                    {"pairingCode",{{"type","string"},{"minLength",1},{"maxLength",maxPairingCode},
                                    {"description","Short-lived code from the authenticated Flock pairing screen."}}},
                    {"requestId",{{"type","string"},{"minLength",1},{"maxLength",maxRequestId},{"pattern","^[A-Za-z0-9._:-]+$"},
                                  {"description","Unique request ID. Retry with the same exact arguments."}}},
                    {"text",{{"type","string"},{"minLength",1},{"maxLength",maxRequestText},
                             {"description","The request for the paired Bot."}}}
                }}
            }}};
}

bool supportedProtocol(std::string_view value) {
    return value == mcpProtocol || value == "2025-06-18";
}

void mcpRequest(const WebCallbacks& callbacks, const httplib::Request& req, httplib::Response& res) {
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
    if (method == "tools/list") return rpcResult(res,*id,{{"tools",Json::array({toolDefinition()})}});
    if (method != "tools/call") return rpcError(res,200,*id,-32601,"Method not found");
    if (params.value("name",Json(nullptr)) != requestTool)
        return rpcError(res,200,*id,-32602,"Unknown tool");

    try {
        const auto args = params.find("arguments");
        if (args == params.end() || !args->is_object() || args->size() != 3) throw InvalidHandoffInput{};
        const auto requestId = identifier(*args,"requestId",maxRequestId);
        if (requestId.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-") != requestId.npos)
            throw InvalidHandoffInput{};
        const auto ack = callbacks.sessionHandoffs->intake(identifier(*args,"pairingCode",maxPairingCode),
            requestId,requestText(*args,"text"));
        const auto data = Json{{"channelId",ack.channel},{"requestId",ack.requestId}};
        return rpcResult(res,*id,{{"structuredContent",data},
            {"content",toolContent("Request " + ack.requestId + " accepted. Flock will send any reply to the paired Thread.")}});
    } catch (const InvalidHandoffInput&) {
        rpcError(res,200,*id,-32602,"Supply pairingCode, requestId and text");
    } catch (const Conflict&) {
        rpcResult(res,*id,{{"isError",true},{"content",toolContent("Flock could not accept this pairing or request.")}});
    } catch (const std::exception&) {
        rpcError(res,200,*id,-32603,"Flock could not record the request");
    }
}
}

void sessionHandoffRoutes(httplib::Server& server, const WebCallbacks& callbacks) {
    server.Post("/mcp/flock",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        mcpRequest(callbacks,req,res);
    });
    server.Get("/api/session-handoffs",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        withHandoff(callbacks,req,res,[&](SessionHandoff& handoff, const Person& actor) {
            auto list = Json::array();
            for (const auto& view : handoff.recent(actor)) {
                const auto& origin = view.origin.invitation;
                list.push_back({{"channelId",view.channel},{"botId",origin.botId},{"threadId",origin.target.threadId},
                                {"mode",view.coordination ? "coordinate" : "consult"},{"state",view.state}});
            }
            reply(res,200,list);
        });
    });
    server.Post("/api/session-handoffs/pair",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        withHandoff(callbacks,req,res,[&](SessionHandoff& handoff, const Person& actor) {
            const auto body = Json::parse(req.body,nullptr,false);
            if (!body.is_object()) throw InvalidHandoffInput{};
            const auto mode = identifier(body,"mode",10);
            const auto target = body.find("target");
            if (target == body.end() || !target->is_object() || target->size() != 3) throw InvalidHandoffInput{};
            const ThreadTarget destination{identifier(*target,"environmentId"),identifier(*target,"projectId"),
                                           identifier(*target,"threadId")};
            const auto bot = identifier(body,"botId");
            std::optional<CoordinationRequest> permission;
            if (mode == "coordinate") {
                const auto delegate = body.find("allowDelegation");
                if (body.size() != 5 || delegate == body.end() || !delegate->is_boolean()) throw InvalidHandoffInput{};
                permission = CoordinationRequest{requestText(body,"goal"),delegate->get<bool>()};
            } else if (mode != "consult" || body.size() != 3) throw InvalidHandoffInput{};
            const auto bots = callbacks.bots();
            if (!std::ranges::any_of(bots,[&](const auto& entry) {
                return entry.is_object() && entry.value("id",Json(nullptr)) == bot;
            })) throw Conflict("Unknown Bot");
            const auto ticket = handoff.pair(actor,destination,bot,permission);
            reply(res,201,{{"code",ticket.code},{"expiresAt",ticket.expiresAt}});
        });
    });
    server.Get(R"(/api/session-handoffs/([0-9]+))",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        withHandoff(callbacks,req,res,[&](SessionHandoff& handoff, const Person& actor) {
            reply(res,200,handoffView(handoff.view(actor,channelId(req))));
        });
    });
    server.Post(R"(/api/session-handoffs/([0-9]+)/answer)",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        withHandoff(callbacks,req,res,[&](SessionHandoff& handoff, const Person& actor) {
            const auto body = objectBody(req,3);
            const auto generation = body.find("generation");
            if (generation == body.end() || !generation->is_number_integer() || generation->get<Id>() <= 0)
                throw InvalidHandoffInput{};
            reply(res,200,handoffView(handoff.answer(actor,channelId(req),generation->get<Id>(),
                                                     identifier(body,"eventId"),requestText(body,"text"))));
        });
    });
    server.Post(R"(/api/session-handoffs/([0-9]+)/work-answer)",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        withHandoff(callbacks,req,res,[&](SessionHandoff& handoff, const Person& actor) {
            const auto body = objectBody(req,5);
            const auto generation = body.find("generation");
            const auto wait = body.find("waitId");
            const auto stage = body.find("stageId");
            if (generation == body.end() || !generation->is_number_integer() || generation->get<Id>() <= 0 ||
                wait == body.end() || !wait->is_number_integer() || wait->get<Id>() <= 0 ||
                stage == body.end() || (!stage->is_null() && !stage->is_string())) throw InvalidHandoffInput{};
            const auto stageId = stage->is_null() ? std::string() : identifier(body,"stageId",80);
            reply(res,200,handoffView(handoff.answerWork(actor,channelId(req),generation->get<Id>(),
                identifier(body,"delegationId",128),stageId,wait->get<Id>(),requestText(body,"text",8000))));
        });
    });
    server.Post(R"(/api/session-handoffs/([0-9]+)/(resume|pause|freeze))",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        withHandoff(callbacks,req,res,[&](SessionHandoff& handoff, const Person& actor) {
            objectBody(req,0);
            const auto channel = channelId(req);
            const auto action = req.matches[2].str();
            if (action == "resume") return reply(res,200,handoffView(handoff.resume(actor,channel)));
            if (action == "pause") return reply(res,200,handoffView(handoff.pause(actor,channel)));
            reply(res,200,handoffView(handoff.freeze(actor,channel)));
        });
    });
}
}
