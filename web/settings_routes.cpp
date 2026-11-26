#include "settings_routes.hpp"
#include "flock/bot_id.hpp"
#include "flock/decision_errors.hpp"
#include "flock/t3_connection.hpp"
#include "flock/json.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/session_question.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace flockv0::web {
bool hasWebIdentities(const WebCallbacks& callbacks) {
    return callbacks.identityRequired ? callbacks.identityRequired() : bool(callbacks.authenticate);
}

Person person(const WebCallbacks& callbacks, const httplib::Request& request) {
    if (!callbacks.authenticate) return request.has_header("Authorization") ? Person{} : localOwner;
    const auto credentials = request.get_header_value_count("Authorization");
    if (!credentials && callbacks.identityRequired) return callbacks.authenticate({});
    if (credentials != 1) return {};
    return callbacks.authenticate(request.get_header_value("Authorization"));
}

namespace {
constexpr Action personOnlyActions[] = {Action::bot_view, Action::bot_talk, Action::bot_decide, Action::bot_settings, Action::rules_manage};

void reply(httplib::Response& res, int status, const Json& value) {
    res.status = status;
    res.set_content(value.dump(), "application/json");
}

std::optional<T3ConnectRequest> parseT3ConnectRequest(const Json& args) {
    using Source = T3ConnectRequest::Source;
    if (!args.is_object()) return {};
    const auto source = args.at("source").get<std::string>();
    if (source == "local" && args.size() == 1) return T3ConnectRequest{Source::Local, {}};
    if (args.size() != 2 || (source != "url" && source != "saved")) return {};
    const bool saved = source == "saved";
    auto value = args.at(saved ? "environmentId" : "mcpUrl").get<std::string>();
    if (value.empty() || value.size() > 4096) return {};
    return T3ConnectRequest{saved ? Source::Saved : Source::Url, std::move(value)};
}
}

void settingsRoutes(httplib::Server& server, Store& store, const WebCallbacks& callbacks) {
    server.Get("/api/settings", [&](const httplib::Request& req, httplib::Response& res) {
        const auto actor = person(callbacks, req);
        Policy policy{store};
        const bool canManage = policy.allows(actor, Action::rules_manage, "installation");
        Json model = nullptr;
        if (canManage && callbacks.modelStatus) {
            try {
                const auto status = callbacks.modelStatus();
                model = {{"connected", status.value("connected", false)},
                         {"account", status.value("account", "")},
                         {"planUsage", status.value("planUsage", false)},
                         {"demo", status.value("demo", false)},
                         {"needsSignIn", status.value("needsSignIn", false)},
                         {"unavailable", status.value("unavailable", false)},
                         {"signingIn", status.value("signingIn", false)},
                         {"signInError", status.value("signInError", "")},
                         {"canSignIn", bool(callbacks.modelLogin)}};
                if (callbacks.modelSwitch && (status.value("connected", false) || status.value("registered", false)))
                    model["canSwitch"] = true;
                if (callbacks.modelLogout) model["canSignOut"] = true;
            } catch (const std::exception&) {
                model = {{"unavailable", true}};
            }
        }
        const auto t3Setup = canManage && callbacks.t3Setup ? callbacks.t3Setup(actor) : Json(nullptr);
        const T3Connections connections(store);
        const auto connection = connections.settings(actor.id, actor.adminRoot);
        const bool canDisconnectT3 = canManage && bool(callbacks.t3Disconnect);
        const bool t3CleanupPending = canDisconnectT3 && connections.providerCleanupPending();
        auto environments = Json::array();
        if (connection) {
            for (const auto& id : connection->environments) {
                environments.push_back(
                    {{"environmentId", id}, {"authorization", callbacks.t3Connection ? callbacks.t3Connection(id) : Json(nullptr)}});
            }
        }
        auto personActionNames = Json::array();
        for (const auto action : personOnlyActions) personActionNames.push_back(actionName(action));
        reply(res, 200,
              {{"mode", policy.mode()},
               {"rules", policy.redactSecrets({}, policy.rules())},
               {"canManagePolicy", canManage},
               {"model", std::move(model)},
               {"t3Setup", t3Setup},
               {"personOnlyActions", std::move(personActionNames)},
               {"canDisconnectT3", canDisconnectT3},
               {"t3CleanupPending", t3CleanupPending},
               {"t3", connection ? Json{{"port", connection->port}, {"environments", std::move(environments)}} : Json(nullptr)}});
    });
    server.Get("/api/settings/models", [&](const httplib::Request& req, httplib::Response& res) {
        const auto bot = req.get_param_value("bot");
        if (req.get_param_value_count("bot") != 1 || !validBotId(bot)) return reply(res, 400, {{"error", "invalid_bot"}});
        if (!Policy{store}.allows(person(callbacks, req), Action::bot_settings, bot)) return reply(res, 403, {{"error", "access_denied"}});
        Json catalog = {{"fallbackReasoningEfforts", fallbackReasoningEfforts()}};
        try {
            if (callbacks.modelCatalog) {
                catalog["models"] = callbacks.modelCatalog();
                return reply(res, 200, catalog);
            }
        } catch (const std::exception&) {
        }
        catalog["error"] = "models_unavailable";
        reply(res, 503, catalog);
    });
    server.Get("/api/settings/installation", [&](const httplib::Request& req, httplib::Response& res) {
        const auto actor = person(callbacks, req);
        if (!Policy{store}.allows(actor, Action::rules_manage, "installation")) return reply(res, 403, {{"error", "access_denied"}});
        if (!callbacks.installationSettings) return reply(res, 404, {{"error", "settings_unavailable"}});
        reply(res, 200, callbacks.installationSettings(actor));
    });
    server.Post(R"(/api/settings/installation/(access|system|github|channels|tools))",
                [&](const httplib::Request& req, httplib::Response& res) {
                    const auto actor = person(callbacks, req);
                    if (!Policy{store}.allows(actor, Action::rules_manage, "installation"))
                        return reply(res, 403, {{"error", "access_denied"}});
                    if (!callbacks.changeInstallationSettings) return reply(res, 404, {{"error", "settings_unavailable"}});
                    if (req.body.size() > 32 * 1024) return reply(res, 413, {{"error", "payload_too_large"}});
                    const auto args = Json::parse(req.body, nullptr, false);
                    if (!args.is_object()) return reply(res, 400, {{"error", "invalid_input"}});
                    try {
                        auto result = callbacks.changeInstallationSettings(actor, req.matches[1].str(), args);
                        result["ok"] = true;
                        reply(res, 200, result);
                    } catch (const Json::exception&) {
                        reply(res, 400, {{"error", "invalid_input"}});
                    } catch (const DecisionDenied&) {
                        reply(res, 403, {{"error", "access_denied"}});
                    } catch (const Conflict& failure) {
                        reply(res, 409, {{"error", "settings_not_saved"}, {"message", failure.what()}});
                    }
                });
    const auto botChannelAccess = [&store, &callbacks](const Person& actor, std::string_view bot) {
        Policy policy{store};
        if (!validBotId(bot) || !policy.allows(actor, Action::rules_manage, "installation") ||
            !policy.allows(actor, Action::bot_settings, bot))
            return false;
        return std::ranges::any_of(callbacks.bots(), [&](const Json& card) { return card.value("id", std::string{}) == bot; });
    };
    server.Get("/api/bot-settings/channels", [&, botChannelAccess](const httplib::Request& req, httplib::Response& res) {
        if (!callbacks.botChannels) return reply(res, 404, {{"error", "settings_unavailable"}});
        if (req.get_param_value_count("bot") != 1) return reply(res, 400, {{"error", "invalid_bot"}});
        const auto bot = req.get_param_value("bot");
        if (!botChannelAccess(person(callbacks, req), bot)) return reply(res, 403, {{"error", "access_denied"}});
        reply(res, 200, {{"bridges", callbacks.botChannels(bot)}});
    });
    server.Post("/api/bot-settings/channels", [&, botChannelAccess](const httplib::Request& req, httplib::Response& res) {
        if (!callbacks.changeBotChannel) return reply(res, 404, {{"error", "settings_unavailable"}});
        if (req.body.size() > 8192) return reply(res, 413, {{"error", "payload_too_large"}});
        const auto args = Json::parse(req.body, nullptr, false);
        try {
            const auto bot = args.at("botId").get<std::string>();
            const auto actor = person(callbacks, req);
            if (!botChannelAccess(actor, bot)) return reply(res, 403, {{"error", "access_denied"}});
            if (args.at("action") == "setSender") {
                const auto member = args.at("sender").at("personId").get<std::string>();
                const auto target = callbacks.configuredPerson ? callbacks.configuredPerson(member) : std::nullopt;
                if (!target || !Policy{store}.allows(Person{target->id, false}, Action::bot_talk, bot))
                    return reply(res, 400, {{"error", "invalid_sender"}, {"message", "Choose a person who can talk to this Bot."}});
            }
            reply(res, 200, {{"bridges", callbacks.changeBotChannel(bot, args)}});
        } catch (const Json::exception&) {
            reply(res, 400, {{"error", "invalid_input"}});
        } catch (const DecisionDenied&) {
            reply(res, 403, {{"error", "access_denied"}});
        } catch (const Conflict& failure) {
            reply(res, 409, {{"error", "settings_not_saved"}, {"message", failure.what()}});
        }
    });
    server.Get("/api/bot-settings", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_param_value_count("bot") != 1 || req.get_param_value("bot").empty()) return reply(res, 400, {{"error", "invalid_bot"}});
        try {
            auto view = Policy{store}.botSettings(person(callbacks, req), req.get_param_value("bot"));
            view["canManageMembers"] = view.at("canManage").get<bool>() && hasWebIdentities(callbacks);
            reply(res, 200, view);
        } catch (const Conflict&) {
            reply(res, 403, {{"error", "access_denied"}});
        }
    });
    server.Post("/api/settings/model-login", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 1024) return reply(res, 413, {{"error", "payload_too_large"}});
        try {
            const auto args = Json::parse(req.body);
            const auto actor = person(callbacks, req);
            if (!Policy{store}.allows(actor, Action::rules_manage, "installation")) throw DecisionDenied{};
            if (!args.is_object() || args.size() != (args.contains("switchAccount") ? 2 : 1))
                return reply(res, 400, {{"error", "invalid_input"}});
            const auto reconsent = args.at("reconsent").get<bool>();
            const auto switchAccount = args.value("switchAccount", false);
            if (switchAccount && reconsent) return reply(res, 400, {{"error", "invalid_input"}});
            if (switchAccount ? !callbacks.modelSwitch : !callbacks.modelLogin) return reply(res, 409, {{"error", "sign_in_unavailable"}});
            if (!(switchAccount ? callbacks.modelSwitch(actor) : callbacks.modelLogin(actor, reconsent)))
                return reply(res, 409, {{"error", "sign_in_pending"}});
            reply(res, 202, {{"ok", true}});
        } catch (const Json::exception&) {
            reply(res, 400, {{"error", "invalid_input"}});
        } catch (const DecisionDenied&) {
            reply(res, 403, {{"error", "access_denied"}});
        } catch (const std::exception&) {
            reply(res, 503, {{"error", "sign_in_unavailable"}});
        }
    });
    server.Post("/api/settings/model-logout", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 1024) return reply(res, 413, {{"error", "payload_too_large"}});
        const auto args = Json::parse(req.body, nullptr, false);
        if (!args.is_object() || !args.empty()) return reply(res, 400, {{"error", "invalid_input"}});
        if (!Policy{store}.allows(person(callbacks, req), Action::rules_manage, "installation"))
            return reply(res, 403, {{"error", "access_denied"}});
        if (!callbacks.modelLogout) return reply(res, 409, {{"error", "sign_out_unavailable"}});
        try {
            const auto revoked = callbacks.modelLogout();
            if (!revoked) return reply(res, 409, {{"error", "sign_in_pending"}});
            reply(res, 200, {{"ok", true}, {"revocationConfirmed", *revoked}});
        } catch (const std::exception&) {
            reply(res, 503, {{"error", "sign_out_unavailable"}});
        }
    });
    server.Post("/api/settings/t3-connect", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 8192) return reply(res, 413, {{"error", "payload_too_large"}});
        try {
            const auto actor = person(callbacks, req);
            if (!Policy{store}.allows(actor, Action::rules_manage, "installation")) throw DecisionDenied{};
            const auto target = parseT3ConnectRequest(Json::parse(req.body));
            if (!target) return reply(res, 400, {{"error", "invalid_input"}});
            if (!callbacks.t3Connect) return reply(res, 409, {{"error", "connect_unavailable"}});
            if (!callbacks.t3Connect(actor, *target)) return reply(res, 409, {{"error", "connect_pending"}});
            reply(res, 202, {{"ok", true}});
        } catch (const Json::exception&) {
            reply(res, 400, {{"error", "invalid_input"}});
        } catch (const DecisionDenied&) {
            reply(res, 403, {{"error", "access_denied"}});
        } catch (const Conflict&) {
            reply(res, 400, {{"error", "invalid_input"}});
        } catch (const std::exception&) {
            reply(res, 503, {{"error", "connect_unavailable"}});
        }
    });
    server.Post("/api/settings/t3-disconnect", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 8192) return reply(res, 413, {{"error", "payload_too_large"}});
        try {
            if (!Policy{store}.allows(person(callbacks, req), Action::rules_manage, "installation")) throw DecisionDenied{};
            const auto args = Json::parse(req.body);
            if (!args.is_object() || args.size() != 1) return reply(res, 400, {{"error", "invalid_input"}});
            const auto& value = args.at("environmentId");
            std::optional<std::string> environment;
            if (!value.is_null()) {
                environment = value.get<std::string>();
                if (environment->empty() || environment->size() > 1024) return reply(res, 400, {{"error", "invalid_input"}});
            }
            if (!callbacks.t3Disconnect) return reply(res, 409, {{"error", "disconnect_unavailable"}});
            callbacks.t3Disconnect(environment);
            reply(res, 200, {{"ok", true}});
        } catch (const Json::exception&) {
            reply(res, 400, {{"error", "invalid_input"}});
        } catch (const DecisionDenied&) {
            reply(res, 403, {{"error", "access_denied"}});
        } catch (const std::exception&) {
            reply(res, 500, {{"error", "disconnect_failed"}});
        }
    });
    server.Post(R"(/api/settings/(mode|rule))", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 4096) return reply(res, 413, {{"error", "payload_too_large"}});
        try {
            const auto args = Json::parse(req.body);
            const auto actor = person(callbacks, req);
            Policy policy{store};
            if (!policy.allows(actor, Action::rules_manage, "installation")) throw DecisionDenied{};
            if (req.matches[1] == "mode")
                policy.setMode(actor, args.at("mode").get<std::string>());
            else {
                const auto action = parseAction(args.at("action").get<std::string>());
                const auto kind = parseRuleKind(args.at("kind").get<std::string>());
                if (!action || !kind) return reply(res, 400, {{"error", "invalid_input"}});
                const auto target = args.at("target").get<std::string>();
                const auto add = args.at("add").get<bool>();
                if (add && (trimSessionQuestionText(target).empty() ||
                            std::ranges::find(personOnlyActions, *action) != std::end(personOnlyActions)))
                    return reply(res, 400, {{"error", "invalid_input"}});
                policy.changeRule(actor, *kind, *action, target, add);
            }
            reply(res, 200, {{"ok", true}});
        } catch (const Json::exception&) {
            reply(res, 400, {{"error", "invalid_input"}});
        } catch (const DecisionDenied&) {
            reply(res, 403, {{"error", "access_denied"}});
        } catch (const Conflict&) {
            reply(res, 400, {{"error", "invalid_input"}});
        }
    });
    server.Post(R"(/api/bot-settings/(members|secret))", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 128 * 1024) return reply(res, 413, {{"error", "payload_too_large"}});
        try {
            const auto args = Json::parse(req.body);
            const auto actor = person(callbacks, req);
            const auto bot = args.at("botId").get<std::string>();
            if (!args.is_object() || args.size() != 3) return reply(res, 400, {{"error", "invalid_input"}});
            Policy policy{store};
            if (req.matches[1] == "members") {
                if (!hasWebIdentities(callbacks)) return reply(res, 409, {{"error", "configure_web_identities_first"}});
                const auto member = args.at("person").get<std::string>();
                const auto add = args.at("add").get<bool>();
                if (add && callbacks.configuredPerson && !callbacks.configuredPerson(member))
                    return reply(res, 400, {{"error", "unknown_person"}});
                policy.setMember(actor, bot, member, add);
            } else {
                const auto& id = args.at("requestId");
                if (!id.is_number_integer() || id.get<Id>() <= 0) return reply(res, 400, {{"error", "invalid_input"}});
                policy.submitSecret(actor, bot, id.get<Id>(), args.at("value").get_ref<const std::string&>());
            }
            reply(res, 200, {{"ok", true}});
        } catch (const Json::exception&) {
            reply(res, 400, {{"error", "invalid_input"}});
        } catch (const Conflict&) {
            reply(res, 403, {{"error", "access_denied"}});
        }
    });
}
}
