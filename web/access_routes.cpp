#include "access_routes.hpp"

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
}

void accessRoutes(httplib::Server& server, Store& store, const WebCallbacks& callbacks) {
    server.Get("/api/access",[&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_param_value_count("bot") != 1 || req.get_param_value("bot").empty())
            return reply(res,400,{{"error","invalid_bot"}});
        try {
            auto view = store.policy().administration(person(callbacks,req),req.get_param_value("bot"));
            view["canManageMembers"] = view.at("canManage").get<bool>() && bool(callbacks.authenticate);
            reply(res,200,view);
        } catch (const Conflict&) { reply(res,403,{{"error","access_denied"}}); }
    });
    server.Post(R"(/api/access/(members|secret))",[&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 128 * 1024) return reply(res,413,{{"error","payload_too_large"}});
        try {
            const auto args = Json::parse(req.body);
            const auto actor = person(callbacks,req);
            const auto bot = args.at("botId").get<std::string>();
            if (!args.is_object() || args.size()!=3) return reply(res,400,{{"error","invalid_input"}});
            auto policy = store.policy();
            if (req.matches[1] == "members") {
                if (!callbacks.authenticate) return reply(res,409,{{"error","configure_web_identities_first"}});
                const auto member = args.at("person").get<std::string>();
                const auto add = args.at("add").get<bool>();
                if (add && callbacks.configuredPerson && !callbacks.configuredPerson(member))
                    return reply(res,400,{{"error","unknown_person"}});
                policy.setMember(actor,bot,member,add);
            } else {
                const auto& id = args.at("requestId");
                if (!id.is_number_integer() || id.get<Id>() <= 0) return reply(res,400,{{"error","invalid_input"}});
                policy.submitSecret(actor,bot,id.get<Id>(),args.at("value").get_ref<const std::string&>());
            }
            reply(res,200,{{"ok",true}});
        } catch (const Json::exception&) { reply(res,400,{{"error","invalid_input"}}); }
        catch (const Conflict&) { reply(res,403,{{"error","access_denied"}}); }
    });
}
}
