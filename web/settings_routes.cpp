#include "settings_routes.hpp"
#include "flock/board_inbox.hpp"
#include "flock/t3_connection.hpp"

#include <algorithm>

namespace flockv0::web {
Person person(const WebCallbacks& callbacks, const httplib::Request& request) {
    if (!callbacks.authenticate) return request.has_header("Authorization") ? Person{} : localOwner;
    if (request.get_header_value_count("Authorization") != 1) return {};
    return callbacks.authenticate(request.get_header_value("Authorization"));
}

namespace {
constexpr Action personOnlyActions[] = {Action::bot_view,Action::bot_talk,Action::bot_decide,
    Action::bot_settings,Action::rules_manage};

void reply(httplib::Response& res, int status, const Json& value) {
    res.status = status;
    res.set_content(value.dump(),"application/json");
}
}

void settingsRoutes(httplib::Server& server, Store& store, const WebCallbacks& callbacks) {
    server.Get("/api/settings",[&](const httplib::Request& req, httplib::Response& res) {
        const auto actor = person(callbacks,req);
        auto policy = store.policy();
        const bool canManage = policy.allows(actor,Action::rules_manage,"installation");
        Json model = nullptr;
        if (canManage && callbacks.modelStatus) {
            try {
                const auto status = callbacks.modelStatus();
                model = {{"connected",status.value("connected",false)},{"account",status.value("account","")},
                         {"planUsage",status.value("planUsage",false)},{"demo",status.value("demo",false)},
                         {"needsSignIn",status.value("needsSignIn",false)},{"unavailable",status.value("unavailable",false)},
                         {"signingIn",status.value("signingIn",false)},{"signInError",status.value("signInError","")},
                         {"canSignIn",bool(callbacks.modelLogin)}};
            } catch (const std::exception&) { model = {{"unavailable",true}}; }
        }
        const auto t3Setup = canManage && callbacks.t3Setup ? callbacks.t3Setup(actor) : Json(nullptr);
        const T3Connections connections(store);
        const auto connection = connections.settings(actor.id,actor.adminRoot);
        const bool canDisconnectT3 = canManage && bool(callbacks.t3Disconnect);
        const bool t3CleanupPending = canDisconnectT3 && connections.providerCleanupPending();
        auto personActionNames = Json::array();
        for (const auto action : personOnlyActions) personActionNames.push_back(actionName(action));
        reply(res,200,{{"mode",policy.mode()},{"rules",policy.redactSecrets({},policy.rules())},
                       {"canManagePolicy",canManage},{"model",std::move(model)},{"t3Setup",t3Setup},
                       {"personOnlyActions",std::move(personActionNames)},
                       {"canDisconnectT3",canDisconnectT3},{"t3CleanupPending",t3CleanupPending},
                       {"t3",connection ? Json{{"port",connection->port},{"environments",connection->environments}}
                                        : Json(nullptr)}});
    });
    server.Get("/api/settings/models",[&](const httplib::Request& req, httplib::Response& res) {
        person(callbacks,req);
        if (!callbacks.modelCatalog) return reply(res,503,{{"error","models_unavailable"}});
        try { reply(res,200,{{"models",callbacks.modelCatalog()}}); }
        catch (const std::exception&) { reply(res,503,{{"error","models_unavailable"}}); }
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
            if (!args.is_object() || args.size() != 1) return reply(res,400,{{"error","invalid_input"}});
            const auto reconsent = args.at("reconsent").get<bool>();
            if (!callbacks.modelLogin) return reply(res,409,{{"error","sign_in_unavailable"}});
            if (!callbacks.modelLogin(reconsent)) return reply(res,409,{{"error","sign_in_pending"}});
            reply(res,202,{{"ok",true}});
        } catch (const Json::exception&) { reply(res,400,{{"error","invalid_input"}}); }
        catch (const DecisionDenied&) { reply(res,403,{{"error","access_denied"}}); }
        catch (const std::exception&) { reply(res,503,{{"error","sign_in_unavailable"}}); }
    });
    server.Post("/api/settings/t3-connect",[&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 8192) return reply(res,413,{{"error","payload_too_large"}});
        try {
            const auto actor = person(callbacks,req);
            if (!store.policy().allows(actor,Action::rules_manage,"installation")) throw DecisionDenied{};
            const auto args = Json::parse(req.body);
            if (!args.is_object() || args.size() != 2) return reply(res,400,{{"error","invalid_input"}});
            const auto environment = args.at("environmentId").get<std::string>();
            const auto url = args.at("mcpUrl").get<std::string>();
            if (environment.empty() || url.empty()) return reply(res,400,{{"error","invalid_input"}});
            if (!callbacks.t3Connect) return reply(res,409,{{"error","connect_unavailable"}});
            if (!callbacks.t3Connect(actor,environment,url)) return reply(res,409,{{"error","connect_pending"}});
            reply(res,202,{{"ok",true}});
        } catch (const Json::exception&) { reply(res,400,{{"error","invalid_input"}}); }
        catch (const DecisionDenied&) { reply(res,403,{{"error","access_denied"}}); }
        catch (const Conflict&) { reply(res,400,{{"error","invalid_input"}}); }
        catch (const std::exception&) { reply(res,503,{{"error","connect_unavailable"}}); }
    });
    server.Post("/api/settings/t3-disconnect",[&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 8192) return reply(res,413,{{"error","payload_too_large"}});
        try {
            if (!store.policy().allows(person(callbacks,req),Action::rules_manage,"installation")) throw DecisionDenied{};
            const auto args = Json::parse(req.body);
            if (!args.is_object() || args.size() != 1) return reply(res,400,{{"error","invalid_input"}});
            const auto& value = args.at("environmentId");
            std::optional<std::string> environment;
            if (!value.is_null()) {
                environment = value.get<std::string>();
                if (environment->empty() || environment->size() > 1024) return reply(res,400,{{"error","invalid_input"}});
            }
            if (!callbacks.t3Disconnect) return reply(res,409,{{"error","disconnect_unavailable"}});
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
                if (add && (trimSessionQuestionText(target).empty() ||
                    std::ranges::find(personOnlyActions,*action) != std::end(personOnlyActions)))
                    return reply(res,400,{{"error","invalid_input"}});
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
