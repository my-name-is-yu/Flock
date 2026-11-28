#include "settings_routes.hpp"
#include "http.hpp"
#include "flock/bot_id.hpp"
#include "flock/bot_profile.hpp"
#include "flock/t3.hpp"
#include "flock/t3_connection.hpp"
#include "flock/json.hpp"
#include "flock/person.hpp"
#include "flock/bot_access.hpp"
#include "flock/policy.hpp"
#include "flock/policy_rules.hpp"
#include "flock/secret_requests.hpp"
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
namespace {
std::vector<std::string> projectEnvironments(Store& store, const WebCallbacks& callbacks, const Person& actor) {
    if (callbacks.demoMode) return {"demo"};
    auto connection = T3Connections(store).settings(actor.id, actor.adminRoot);
    return connection ? std::move(connection->environments) : std::vector<std::string>{};
}

std::vector<T3Project> availableProjects(Store& store, const WebCallbacks& callbacks, const Person& actor, std::string_view environment) {
    const auto environments = projectEnvironments(store, callbacks, actor);
    if (!callbacks.sessionT3 || std::ranges::find(environments, environment) == environments.end()) return {};
    return callbacks.sessionT3->projects(environment);
}

constexpr Action personOnlyActions[] = {Action::bot_view, Action::bot_talk, Action::bot_decide, Action::rules_manage};

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

bool projectAvailable(Store& store, const WebCallbacks& callbacks, const Person& actor, std::string_view environment,
                      std::string_view project) {
    const auto projects = availableProjects(store, callbacks, actor, environment);
    return std::ranges::find(projects, project, &T3Project::id) != projects.end();
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
        Json remote = nullptr;
        if (canManage) {
            auto machines = Json::array();
            for (const auto& machine : connections.remoteMachines()) machines.push_back({{"id", machine.id}, {"name", machine.name}});
            if (connection || !machines.empty()) {
                const auto url = callbacks.remoteSessionMcpUrl ? callbacks.remoteSessionMcpUrl() : std::string{};
                remote = {{"url", url.empty() ? Json(nullptr) : Json(url)}, {"machines", std::move(machines)}};
            }
        }
        auto personActionNames = Json::array();
        for (const auto action : personOnlyActions) personActionNames.push_back(actionName(action));
        reply(res, 200,
              {{"mode", policy.mode()},
               {"rules", policy.redactSecrets({}, PolicyRules{store}.rules())},
               {"canManagePolicy", canManage},
               {"model", std::move(model)},
               {"t3Setup", t3Setup},
               {"personOnlyActions", std::move(personActionNames)},
               {"canDisconnectT3", canDisconnectT3},
               {"t3CleanupPending", t3CleanupPending},
               {"remoteMachines", std::move(remote)},
               {"t3", connection ? Json{{"port", connection->port}, {"environments", std::move(environments)}} : Json(nullptr)}});
    });
    server.Get("/api/settings/projects", [&](const httplib::Request& req, httplib::Response& res) {
        const auto actor = person(callbacks, req);
        const Policy policy(store);
        if (!policy.allows(actor, Action::bot_create, "")) return error(res, 403, "access_denied");
        if (req.has_param("environmentId")) {
            const auto environment = req.get_param_value("environmentId");
            if (req.get_param_value_count("environmentId") != 1 || !validPlainId(environment)) return error(res, 400, "invalid_workspace");
            try {
                auto projects = Json::array();
                for (const auto& project : availableProjects(store, callbacks, actor, environment)) {
                    if (validPlainId(project.id)) projects.push_back({{"id", project.id}, {"name", project.title}});
                }
                reply(res, 200, policy.redactSecrets({}, {{"projects", std::move(projects)}}));
            } catch (const std::exception&) {
                error(res, 503, "workspaces_unavailable");
            }
        } else {
            auto environments = Json::array();
            for (const auto& id : projectEnvironments(store, callbacks, actor))
                environments.push_back({{"id", id}, {"name", callbacks.demoMode ? "Demo" : id}});
            reply(res, 200,
                  policy.redactSecrets(
                      {}, {{"environments", std::move(environments)}, {"recent", BotAccess(store).recentProjectAssignment(actor)}}));
        }
    });
    server.Get("/api/settings/models", [&](const httplib::Request& req, httplib::Response& res) {
        const auto bot = req.get_param_value("bot");
        if (req.get_param_value_count("bot") != 1 || !validBotId(bot)) return error(res, 400, "invalid_bot");
        if (!Policy{store}.allows(person(callbacks, req), Action::bot_settings, bot)) return error(res, 403, "access_denied");
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
        if (!Policy{store}.allows(actor, Action::rules_manage, "installation")) return error(res, 403, "access_denied");
        if (!callbacks.installationSettings) return error(res, 404, "settings_unavailable");
        reply(res, 200, callbacks.installationSettings(actor));
    });
    server.Post(R"(/api/settings/installation/(access|system|github|channels|tools))",
                [&](const httplib::Request& req, httplib::Response& res) {
                    const auto actor = person(callbacks, req);
                    if (!Policy{store}.allows(actor, Action::rules_manage, "installation")) return error(res, 403, "access_denied");
                    if (!callbacks.changeInstallationSettings) return error(res, 404, "settings_unavailable");
                    if (req.body.size() > 32 * 1024) return error(res, 413, "payload_too_large");
                    const auto args = Json::parse(req.body, nullptr, false);
                    if (!args.is_object()) return error(res, 400, "invalid_input");
                    try {
                        auto result = callbacks.changeInstallationSettings(actor, req.matches[1].str(), args);
                        result["ok"] = true;
                        reply(res, 200, result);
                    } catch (const Json::exception&) {
                        error(res, 400, "invalid_input");
                    } catch (const AccessDenied&) {
                        error(res, 403, "access_denied");
                    } catch (const Conflict& failure) {
                        error(res, 409, "settings_not_saved", failure.what());
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
        if (!callbacks.botChannels) return error(res, 404, "settings_unavailable");
        if (req.get_param_value_count("bot") != 1) return error(res, 400, "invalid_bot");
        const auto bot = req.get_param_value("bot");
        if (!botChannelAccess(person(callbacks, req), bot)) return error(res, 403, "access_denied");
        reply(res, 200, {{"bridges", callbacks.botChannels(bot)}});
    });
    server.Post("/api/bot-settings/channels", [&, botChannelAccess](const httplib::Request& req, httplib::Response& res) {
        if (!callbacks.changeBotChannel) return error(res, 404, "settings_unavailable");
        if (req.body.size() > 8192) return error(res, 413, "payload_too_large");
        const auto args = Json::parse(req.body, nullptr, false);
        try {
            const auto bot = args.at("botId").get<std::string>();
            const auto actor = person(callbacks, req);
            if (!botChannelAccess(actor, bot)) return error(res, 403, "access_denied");
            if (args.at("action") == "setSender") {
                const auto member = args.at("sender").at("personId").get<std::string>();
                const auto target = callbacks.configuredPerson ? callbacks.configuredPerson(member) : std::nullopt;
                if (!target || !Policy{store}.allows(Person{target->id, false}, Action::bot_talk, bot))
                    return error(res, 400, "invalid_sender", "Choose a person who can talk to this Bot.");
            }
            reply(res, 200, {{"bridges", callbacks.changeBotChannel(bot, args)}});
        } catch (const Json::exception&) {
            error(res, 400, "invalid_input");
        } catch (const AccessDenied&) {
            error(res, 403, "access_denied");
        } catch (const Conflict& failure) {
            error(res, 409, "settings_not_saved", failure.what());
        }
    });
    server.Get("/api/bot-settings", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_param_value_count("bot") != 1 || req.get_param_value("bot").empty()) return error(res, 400, "invalid_bot");
        try {
            auto view = BotAccess{store}.botSettings(person(callbacks, req), req.get_param_value("bot"));
            view["canManageMembers"] = view.at("canManage").get<bool>() && hasWebIdentities(callbacks);
            reply(res, 200, view);
        } catch (const Conflict&) {
            error(res, 403, "access_denied");
        }
    });
    server.Post("/api/settings/model-login", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 1024) return error(res, 413, "payload_too_large");
        try {
            const auto args = Json::parse(req.body);
            const auto actor = person(callbacks, req);
            if (!Policy{store}.allows(actor, Action::rules_manage, "installation")) throw AccessDenied{};
            if (!args.is_object() || args.size() != (args.contains("switchAccount") ? 2 : 1)) return error(res, 400, "invalid_input");
            const auto reconsent = args.at("reconsent").get<bool>();
            const auto switchAccount = args.value("switchAccount", false);
            if (switchAccount && reconsent) return error(res, 400, "invalid_input");
            if (switchAccount ? !callbacks.modelSwitch : !callbacks.modelLogin) return error(res, 409, "sign_in_unavailable");
            if (!(switchAccount ? callbacks.modelSwitch(actor) : callbacks.modelLogin(actor, reconsent)))
                return error(res, 409, "sign_in_pending");
            reply(res, 202, {{"ok", true}});
        } catch (const Json::exception&) {
            error(res, 400, "invalid_input");
        } catch (const AccessDenied&) {
            error(res, 403, "access_denied");
        } catch (const std::exception&) {
            error(res, 503, "sign_in_unavailable");
        }
    });
    server.Post("/api/settings/model-logout", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 1024) return error(res, 413, "payload_too_large");
        const auto args = Json::parse(req.body, nullptr, false);
        if (!args.is_object() || !args.empty()) return error(res, 400, "invalid_input");
        if (!Policy{store}.allows(person(callbacks, req), Action::rules_manage, "installation")) return error(res, 403, "access_denied");
        if (!callbacks.modelLogout) return error(res, 409, "sign_out_unavailable");
        try {
            const auto revoked = callbacks.modelLogout();
            if (!revoked) return error(res, 409, "sign_in_pending");
            reply(res, 200, {{"ok", true}, {"revocationConfirmed", *revoked}});
        } catch (const std::exception&) {
            error(res, 503, "sign_out_unavailable");
        }
    });
    server.Post("/api/settings/t3-connect", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 8192) return error(res, 413, "payload_too_large");
        try {
            const auto actor = person(callbacks, req);
            if (!Policy{store}.allows(actor, Action::rules_manage, "installation")) throw AccessDenied{};
            const auto target = parseT3ConnectRequest(Json::parse(req.body));
            if (!target) return error(res, 400, "invalid_input");
            if (!callbacks.t3Connect) return error(res, 409, "connect_unavailable");
            if (!callbacks.t3Connect(actor, *target)) return error(res, 409, "connect_pending");
            reply(res, 202, {{"ok", true}});
        } catch (const Json::exception&) {
            error(res, 400, "invalid_input");
        } catch (const AccessDenied&) {
            error(res, 403, "access_denied");
        } catch (const Conflict&) {
            error(res, 400, "invalid_input");
        } catch (const std::exception&) {
            error(res, 503, "connect_unavailable");
        }
    });
    server.Post("/api/settings/t3-disconnect", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 8192) return error(res, 413, "payload_too_large");
        try {
            if (!Policy{store}.allows(person(callbacks, req), Action::rules_manage, "installation")) throw AccessDenied{};
            const auto args = Json::parse(req.body);
            if (!args.is_object() || args.size() != 1) return error(res, 400, "invalid_input");
            const auto& value = args.at("environmentId");
            std::optional<std::string> environment;
            if (!value.is_null()) {
                environment = value.get<std::string>();
                if (environment->empty() || environment->size() > 1024) return error(res, 400, "invalid_input");
            }
            if (!callbacks.t3Disconnect) return error(res, 409, "disconnect_unavailable");
            callbacks.t3Disconnect(environment);
            reply(res, 200, {{"ok", true}});
        } catch (const Json::exception&) {
            error(res, 400, "invalid_input");
        } catch (const AccessDenied&) {
            error(res, 403, "access_denied");
        } catch (const std::exception&) {
            error(res, 500, "disconnect_failed");
        }
    });
    server.Post("/api/settings/remote-machine", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 4096) return error(res, 413, "payload_too_large");
        try {
            if (!Policy{store}.allows(person(callbacks, req), Action::rules_manage, "installation")) throw AccessDenied{};
            const auto args = Json::parse(req.body);
            if (!args.is_object() || args.size() != 1) return error(res, 400, "invalid_input");
            T3Connections connections(store);
            if (args.contains("remove")) {
                if (!connections.removeRemoteMachine(args.at("remove").get<std::string>())) return error(res, 404, "not_found");
                return reply(res, 200, {{"ok", true}});
            }
            const auto url = callbacks.remoteSessionMcpUrl ? callbacks.remoteSessionMcpUrl() : std::string{};
            if (url.empty()) return error(res, 409, "remote_unavailable");
            const auto credential = connections.addRemoteMachine(args.at("name").get<std::string>());
            reply(res, 201, {{"id", credential.id}, {"url", url}, {"bearerToken", credential.bearerToken}});
        } catch (const Json::exception&) {
            error(res, 400, "invalid_input");
        } catch (const AccessDenied&) {
            error(res, 403, "access_denied");
        } catch (const Conflict& failure) {
            error(res, 409, "invalid_input", failure.what());
        }
    });
    server.Post(R"(/api/settings/(mode|rule))", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > 4096) return error(res, 413, "payload_too_large");
        try {
            const auto args = Json::parse(req.body);
            const auto actor = person(callbacks, req);
            if (!Policy{store}.allows(actor, Action::rules_manage, "installation")) throw AccessDenied{};
            PolicyRules rules{store};
            if (req.matches[1] == "mode")
                rules.setMode(actor, args.at("mode").get<std::string>());
            else {
                const auto action = parseAction(args.at("action").get<std::string>());
                const auto kind = parseRuleKind(args.at("kind").get<std::string>());
                if (!action || !kind) return error(res, 400, "invalid_input");
                const auto target = args.at("target").get<std::string>();
                const auto add = args.at("add").get<bool>();
                if (add && (trimSessionQuestionText(target).empty() ||
                            std::ranges::find(personOnlyActions, *action) != std::end(personOnlyActions)))
                    return error(res, 400, "invalid_input");
                rules.changeRule(actor, *kind, *action, target, add);
            }
            reply(res, 200, {{"ok", true}});
        } catch (const Json::exception&) {
            error(res, 400, "invalid_input");
        } catch (const AccessDenied&) {
            error(res, 403, "access_denied");
        } catch (const Conflict&) {
            error(res, 400, "invalid_input");
        }
    });
    server.Post(R"(/api/bot-settings/(members|secret))", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > maxStandardPayload) return error(res, 413, "payload_too_large");
        try {
            const auto args = Json::parse(req.body);
            const auto actor = person(callbacks, req);
            const auto bot = args.at("botId").get<std::string>();
            if (!args.is_object() || args.size() != 3) return error(res, 400, "invalid_input");
            if (req.matches[1] == "members") {
                if (!hasWebIdentities(callbacks)) return error(res, 409, "configure_web_identities_first");
                const auto member = args.at("person").get<std::string>();
                const auto add = args.at("add").get<bool>();
                if (add && callbacks.configuredPerson && !callbacks.configuredPerson(member)) return error(res, 400, "unknown_person");
                BotAccess{store}.setMember(actor, bot, member, add);
            } else {
                const auto& id = args.at("requestId");
                if (!id.is_number_integer() || id.get<Id>() <= 0) return error(res, 400, "invalid_input");
                SecretRequests{store}.submitSecret(actor, bot, id.get<Id>(), args.at("value").get_ref<const std::string&>());
            }
            reply(res, 200, {{"ok", true}});
        } catch (const Json::exception&) {
            error(res, 400, "invalid_input");
        } catch (const Conflict&) {
            error(res, 403, "access_denied");
        }
    });
}
}
