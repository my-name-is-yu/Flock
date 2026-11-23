#include "settings_routes.hpp"
#include "flock/board_inbox.hpp"
#include "flock/t3_connection.hpp"

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

void settingsRoutes(httplib::Server& server, Store& store, const WebCallbacks& callbacks) {
    server.Get("/api/settings",[&](const httplib::Request& req, httplib::Response& res) {
        const auto actor = person(callbacks,req);
        auto policy = store.policy();
        const auto connection = T3Connections(store).settings(actor.id,actor.adminRoot);
        const bool canManage = policy.allows(actor,Action::rules_manage,"installation");
        // Only installation roots see which model account Flock uses.
        Json model = nullptr;
        if (canManage && callbacks.modelStatus) {
            try {
                const auto status = callbacks.modelStatus();
                model = {{"connected",status.value("connected",false)},{"account",status.value("account","")},
                         {"planUsage",status.value("planUsage",false)},{"demo",status.value("demo",false)},
                         {"signingIn",status.value("signingIn",false)},{"signInError",status.value("signInError","")},
                         {"canSignIn",bool(callbacks.modelLogin)}};
            } catch (const std::exception&) { model = {{"unavailable",true}}; }
        }
        const Json t3Setup = canManage && callbacks.t3Setup ? callbacks.t3Setup() : Json(nullptr);
        reply(res,200,{{"mode",policy.mode()},{"rules",policy.redactSecrets({},policy.rules())},
                       {"canManagePolicy",canManage},{"model",model},{"t3Setup",t3Setup},
                       {"t3",connection ? Json{{"port",connection->port},{"environments",connection->environments}}
                                        : Json(nullptr)}});
    });
    server.Get("/api/bot-settings",[&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_param_value_count("bot") != 1 || req.get_param_value("bot").empty())
            return reply(res,400,{{"error","invalid_bot"}});
        try {
            auto view = store.policy().botSettings(person(callbacks,req),req.get_param_value("bot"));
            view["canManageMembers"] = view.at("canManage").get<bool>() && bool(callbacks.authenticate);
            reply(res,200,view);
        } catch (const Conflict&) { reply(res,403,{{"error","access_denied"}}); }
    });
    server.Post("/api/settings/model-login",[&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 1024) return reply(res,413,{{"error","payload_too_large"}});
        try {
            const auto args = Json::parse(req.body);
            if (!store.policy().allows(person(callbacks,req),Action::rules_manage,"installation")) throw DecisionDenied{};
            if (!callbacks.modelLogin) return reply(res,409,{{"error","sign_in_unavailable"}});
            callbacks.modelLogin(args.at("reconsent").get<bool>());
            reply(res,202,{{"ok",true}});
        } catch (const Json::exception&) { reply(res,400,{{"error","invalid_input"}}); }
        catch (const DecisionDenied&) { reply(res,403,{{"error","access_denied"}}); }
    });
    server.Post("/api/settings/t3-connect",[&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 4096) return reply(res,413,{{"error","payload_too_large"}});
        try {
            const auto args = Json::parse(req.body);
            if (!store.policy().allows(person(callbacks,req),Action::rules_manage,"installation")) throw DecisionDenied{};
            if (!callbacks.t3Connect) return reply(res,409,{{"error","connect_unavailable"}});
            const auto environment = args.at("environmentId").get<std::string>();
            const auto url = args.at("mcpUrl").get<std::string>();
            if (environment.empty() || environment.size() > 200 ||
                !(url.starts_with("https://") || url.starts_with("http://127.0.0.1") || url.starts_with("http://localhost")))
                return reply(res,400,{{"error","invalid_input"}});
            callbacks.t3Connect(environment,url);
            reply(res,202,{{"ok",true}});
        } catch (const Json::exception&) { reply(res,400,{{"error","invalid_input"}}); }
        catch (const DecisionDenied&) { reply(res,403,{{"error","access_denied"}}); }
    });
    server.Post("/api/settings/t3-disconnect",[&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 1024) return reply(res,413,{{"error","payload_too_large"}});
        try {
            const auto args = Json::parse(req.body);
            if (!store.policy().allows(person(callbacks,req),Action::rules_manage,"installation")) throw DecisionDenied{};
            if (!callbacks.t3Disconnect) return reply(res,409,{{"error","disconnect_unavailable"}});
            const auto environment = args.at("environmentId").get<std::string>();
            if (environment.empty() || environment.size() > 200) return reply(res,400,{{"error","invalid_input"}});
            callbacks.t3Disconnect(environment);
            reply(res,200,{{"ok",true}});
        } catch (const Json::exception&) { reply(res,400,{{"error","invalid_input"}}); }
        catch (const DecisionDenied&) { reply(res,403,{{"error","access_denied"}}); }
        catch (const std::exception&) { reply(res,500,{{"error","disconnect_failed"}}); }
    });
    server.Post(R"(/api/settings/(mode|rule))",[&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 4096) return reply(res,413,{{"error","payload_too_large"}});
        try {
            const auto args = Json::parse(req.body);
            const auto actor = person(callbacks,req);
            auto policy = store.policy();
            if (!policy.allows(actor,Action::rules_manage,"installation")) throw DecisionDenied{};
            if (req.matches[1] == "mode") policy.setMode(actor,args.at("mode").get<std::string>());
            else {
                const auto action = parseAction(args.at("action").get<std::string>());
                const auto kind = parseRuleKind(args.at("kind").get<std::string>());
                if (!action || !kind) return reply(res,400,{{"error","invalid_input"}});
                const auto target = args.at("target").get<std::string>();
                const auto add = args.at("add").get<bool>();
                if (add && target.empty()) return reply(res,400,{{"error","invalid_input"}});
                policy.changeRule(actor,*kind,*action,target,add);
            }
            reply(res,200,{{"ok",true}});
        } catch (const Json::exception&) { reply(res,400,{{"error","invalid_input"}}); }
        catch (const DecisionDenied&) { reply(res,403,{{"error","access_denied"}}); }
        catch (const Conflict&) { reply(res,400,{{"error","invalid_input"}}); }
    });
    server.Post(R"(/api/bot-settings/(members|secret))",[&](const httplib::Request& req, httplib::Response& res) {
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
