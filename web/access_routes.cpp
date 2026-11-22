#include "access_routes.hpp"

#include <algorithm>
#include <map>

namespace flockv0::web {
Person person(const WebCallbacks& callbacks, const httplib::Request& request) {
    if (!callbacks.authenticate) return request.has_header("Authorization") ? Person{} : localOwner;
    if (request.get_header_value_count("Authorization") != 1) return {};
    return callbacks.authenticate(request.get_header_value("Authorization"));
}

namespace {
void reply(httplib::Response& res, int status, const Json& value) {
    res.status = status;
    res.set_content(value.dump(),"application/json");
}
Id requestId(const Json& args) {
    const auto& id = args.at("requestId");
    if (!id.is_number_integer() || id.get<Id>() <= 0) throw Conflict("Invalid request ID");
    return id.get<Id>();
}
Role role(const std::string& name) {
    if (name == "view") return Role::view;
    if (name == "decide") return Role::decide;
    if (name == "admin") return Role::admin;
    throw Conflict("Unknown role");
}
}

void accessRoutes(httplib::Server& server, Store& store, const WebCallbacks& callbacks) {
    server.Get("/api/access",[&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_param_value_count("bot") != 1 || req.get_param_value("bot").empty())
            return reply(res,400,{{"error","invalid_bot"}});
        try { reply(res,200,store.access().administration(person(callbacks,req),req.get_param_value("bot"))); }
        catch (const Conflict&) { reply(res,403,{{"error","access_denied"}}); }
    });
    server.Post(R"(/api/access/(person|person-scope|resource|decision|secret|bot-change))",[&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size()>128*1024) return reply(res,413,{{"error","payload_too_large"}});
        try {
            auto args = Json::parse(req.body);
            auto access = store.access();
            auto actor = person(callbacks,req);
            auto bot = args.at("botId").get<std::string>();
            auto action = req.matches[1].str();
            if ((action == "person" || action == "person-scope") && !callbacks.authenticate) {
                reply(res,409,{{"error","configure_web_identities_first"}});
                return;
            }
            if (!access.permitsPerson(actor,bot,Role::admin)) throw Conflict("Denied");
            static const std::map<std::string,std::vector<std::string>> fields{
                {"person",{"botId","action","person","role"}},
                {"person-scope",{"botId","action","person","resourceKind","resourceId","operation"}},
                {"resource",{"botId","action","resourceKind","resourceId","operation"}},
                {"decision",{"botId","requestId","approve"}},{"bot-change",{"botId","requestId","approve"}},
                {"secret",{"botId","requestId","value"}}};
            const auto& expected = fields.at(action);
            if (args.size()!=expected.size() || !std::all_of(expected.begin(),expected.end(),[&](const auto& key){ return args.contains(key); }))
                return reply(res,400,{{"error","invalid_input"}});
            if (action == "person" || action == "person-scope" || action == "resource") {
                const auto change = args.at("action").get<std::string>();
                if (change != "grant" && change != "revoke") throw Conflict("Invalid change");
                const auto changeType = change == "revoke" ? GrantChange::revoke : GrantChange::grant;
                if (action == "person") access.personGrant(actor,bot,args.at("person").get_ref<const std::string&>(),role(args.at("role")),changeType);
                else if (action == "person-scope") access.personScope(actor,args.at("person").get_ref<const std::string&>(),{args.at("resourceKind"),args.at("resourceId"),args.at("operation")},changeType);
                else access.resourceGrant(actor,bot,{args.at("resourceKind"),args.at("resourceId"),args.at("operation")},changeType);
            } else if (action == "secret") {
                access.submitSecret(actor,bot,requestId(args),args.at("value").get_ref<const std::string&>());
            } else if (action == "bot-change") {
                access.reviewBotChange(actor,bot,requestId(args),args.at("approve").get<bool>(),callbacks.applyBotChange);
            } else {
                access.decide(actor,bot,requestId(args),args.at("approve").get<bool>());
            }
            reply(res,200,{{"ok",true}});
        } catch (const Json::exception&) { reply(res,400,{{"error","invalid_input"}}); }
        catch (const BotChangePending&) { reply(res,202,{{"status","approved"}}); }
        catch (const Conflict&) { reply(res,403,{{"error","access_denied"}}); }
    });
}
}
