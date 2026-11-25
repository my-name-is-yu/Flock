#include "channel_routes.hpp"
#include "http_payload.hpp"
#include "settings_routes.hpp"
#include "flock/channels.hpp"

#include <charconv>

namespace flockv0::web {
namespace {
void reply(httplib::Response& res, int status, const Json& body) {
    res.status = status;
    res.set_content(body.dump(-1,' ',false,Json::error_handler_t::replace),"application/json");
}

Id connectionId(const Json& body) {
    const auto& id = body.at("id");
    if (!id.is_number_integer() || id.get<Id>() <= 0) throw Conflict("Invalid connection ID");
    return id.get<Id>();
}
}

bool bridgeEndpoint(std::string_view path) {
    return path == "/api/channels/slack/events" || path == "/api/channels/imessage/events" ||
           path == "/api/channels/local-operations";
}

bool artifactPreview(std::string_view path) {
    constexpr std::string_view prefix = "/api/artifacts/flock-";
    return path.starts_with(prefix) && path.size() == prefix.size() + 32 &&
           path.substr(prefix.size()).find_first_not_of("0123456789abcdef") == path.npos;
}

void channelRoutes(httplib::Server& server, Store& store, const WebCallbacks& callbacks) {
    server.Post(R"(/api/channels/(slack|imessage)/events)",[&](const auto& req, auto& res) {
        if (req.body.size() > maxUploadPayload) return reply(res,413,{{"error","payload_too_large"}});
        const auto kind = req.matches[1].str() == "slack" ? ChannelKind::slack : ChannelKind::imessage;
        if (req.get_header_value_count("Authorization") != 1)
            return reply(res,401,{{"error","channel_identity_required"}});
        try {
            const auto event = Json::parse(req.body);
            if (!event.is_object()) return reply(res,400,{{"error","invalid_event"}});
            reply(res,201,store.receiveChannel(kind,req.get_header_value("Authorization"),event,callbacks.intake));
        } catch (const ChannelAccessDenied&) { reply(res,403,{{"error","channel_access_denied"}}); }
        catch (const Json::exception&) { reply(res,400,{{"error","invalid_event"}}); }
        catch (const Conflict& error) { reply(res,409,{{"error",error.what()}}); }
    });
    server.Get(R"(/api/artifacts/(flock-[0-9a-f]{32}))",[&](const auto& req, auto& res) {
        const auto artifact = store.artifact(req.matches[1].str());
        if (!artifact) return reply(res,404,{{"error","artifact_not_found"}});
        res.headers.erase("Content-Security-Policy");
        res.set_header("Content-Security-Policy","sandbox; default-src 'none'; style-src 'unsafe-inline'; "
                       "img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
        res.set_header("Cross-Origin-Opener-Policy","same-origin");
        res.set_content(artifact->bytes,artifact->mimeType);
    });
    server.Get(R"(/api/channels/attachments/([0-9]+))",[&](const auto& req, auto& res) {
        const auto value = req.matches[1].str();
        Id id = 0;
        const auto parsed = std::from_chars(value.data(),value.data()+value.size(),id);
        const auto attachment = parsed.ec == std::errc{} ? store.attachment(person(callbacks,req),id) : std::nullopt;
        if (!attachment) return reply(res,404,{{"error","attachment_not_found"}});
        if (attachment->mimeType == "text/plain") res.set_header("Content-Disposition","attachment");
        res.set_content(attachment->bytes,attachment->mimeType);
    });
    server.Get("/api/channels/local-connections",[&](const auto& req, auto& res) {
        reply(res,200,store.localConnections(person(callbacks,req)));
    });
    server.Post("/api/channels/local-operations",[&](const auto& req, auto& res) {
        if (!callbacks.authenticateSessionBridge || req.get_header_value_count("Authorization") != 1 ||
            !callbacks.authenticateSessionBridge(req.get_header_value("Authorization")))
            return reply(res,401,{{"error","session_bridge_identity_required"}});
        if (req.body.size() > 8192) return reply(res,413,{{"error","payload_too_large"}});
        try {
            const auto body = Json::parse(req.body);
            const RunRef run{body.at("threadId"),body.at("runId"),{}, {}};
            reply(res,200,store.runLocalMachineOperation(body.at("environmentId").template get<std::string>(),run,
                body.at("machineId").template get<std::string>(),body.at("operation").template get<std::string>(),
                body.at("path").template get<std::string>()));
        } catch (const ChannelAccessDenied&) { reply(res,403,{{"error","session_access_denied"}}); }
        catch (const Json::exception&) { reply(res,400,{{"error","invalid_input"}}); }
        catch (const Conflict& e) { reply(res,409,{{"error",e.what()}}); }
    });
    server.Post(R"(/api/channels/local-connections/(decide|disconnect))",[&](const auto& req, auto& res) {
        if (req.body.size() > 1024) return reply(res,413,{{"error","payload_too_large"}});
        try {
            const auto body = Json::parse(req.body);
            const auto actor = person(callbacks,req);
            if (req.matches[1].str() == "decide") store.decideLocalConnection(actor,connectionId(body),body.at("approve").template get<bool>());
            else store.disconnectLocalConnection(actor,connectionId(body));
            reply(res,200,{{"ok",true}});
        } catch (const ChannelAccessDenied&) { reply(res,403,{{"error","access_denied"}}); }
        catch (const Json::exception&) { reply(res,400,{{"error","invalid_input"}}); }
        catch (const Conflict& error) { reply(res,409,{{"error",error.what()}}); }
    });
}
}
