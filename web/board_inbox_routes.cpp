#include "board_inbox_routes.hpp"
#include "access_routes.hpp"
#include "views.hpp"
#include "flock/board_inbox.hpp"

#include <limits>
#include <map>

namespace flockv0::web {
namespace {
using httplib::Request;
using httplib::Response;

void reply(Response& res, int status, const Json& body) {
    res.status = status;
    res.set_content(body.dump(-1,' ',false,Json::error_handler_t::replace),"application/json; charset=utf-8");
}

void error(Response& res, int status, std::string_view code, std::string_view message = {}) {
    reply(res,status,{{"error",code},{"message",message}});
}

std::string parameter(const Request& request, const char* name) {
    if (request.get_param_value_count(name) > 1) throw std::invalid_argument("Repeated query parameter");
    return request.get_param_value(name);
}

Id targetId(const Json& body, const char* field, Id minimum = 0) {
    const auto& value = body.at(field);
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() > std::numeric_limits<Id>::max()) ||
        value.get<Id>() < minimum) throw std::invalid_argument("Invalid decision target");
    return value.get<Id>();
}

void setBotNames(Json& rows, Store& store, const WebCallbacks& callbacks) {
    if (rows.empty()) return;
    std::map<std::string,std::string,std::less<>> names;
    for (const auto& bot : callbacks.bots()) names.emplace(field(bot,"id"),field(bot,"name"));
    for (auto& row : rows) {
        const auto& id = row.at("botId").get_ref<const std::string&>();
        const auto name = names.find(id);
        if (name != names.end() && !name->second.empty())
            row["botName"] = store.access().redactSecrets(id,name->second);
    }
}
}

void boardInboxRoutes(httplib::Server& server, Store& store, const WebCallbacks& callbacks) {
    for (const auto* path : {"/api/board","/api/inbox","/api/inbox/history"}) {
        server.Get(path,[&store,&callbacks](const Request& req, Response& res) {
            const auto actor = person(callbacks,req);
            try {
                const auto bot = parameter(req,"bot");
                if (req.has_param("bot") && !validBotId(bot)) return error(res,400,"invalid_bot");
                auto rows = req.path == "/api/board" ? store.board(actor,bot,parameter(req,"status"))
                    : req.path == "/api/inbox" ? store.inbox(actor,bot) : store.decisionHistory(actor,bot);
                setBotNames(rows,store,callbacks);
                const auto* key = req.path == "/api/board" ? "tasks" : req.path == "/api/inbox" ? "items" : "decisions";
                reply(res,200,{{key,std::move(rows)}});
            } catch (const std::invalid_argument&) { error(res,400,"invalid_filter"); }
        });
    }

    server.Post("/api/inbox/decide",[&store,&callbacks](const Request& req, Response& res) {
        if (req.body.size() > 128 * 1024) return error(res,413,"payload_too_large");
        try {
            const auto body = Json::parse(req.body);
            const auto kind = parseInboxKind(body.at("itemKind").get<std::string>());
            const InboxTarget target{body.at("botId").get<std::string>(),kind,targetId(body,"itemId",1),
                targetId(body,"waitId"),targetId(body,"taskId"),targetId(body,"targetRevision"),
                targetId(body,"targetGeneration")};
            if (!validBotId(target.botId)) return error(res,400,"invalid_bot");
            store.decideInbox(person(callbacks,req),target,body.at("decision").get<std::string>(),
                              body.value("data",Json::object()),callbacks.applyBotChange);
            reply(res,200,{{"accepted",true}});
        } catch (const Json::exception&) { error(res,400,"invalid_input"); }
        catch (const std::invalid_argument&) { error(res,400,"invalid_input"); }
        catch (const DecisionDenied& e) { error(res,403,"access_denied",e.what()); }
        catch (const RequestChanged& e) { error(res,409,"request_changed",e.what()); }
        catch (const DecisionUnavailable& e) { error(res,409,"already_closed",e.what()); }
        catch (const BotChangePending&) { reply(res,202,{{"accepted",true},{"status","approved"}}); }
        catch (const Conflict&) { error(res,409,"decision_conflict"); }
    });
}
}
