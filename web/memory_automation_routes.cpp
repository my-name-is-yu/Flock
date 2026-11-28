#include "settings_routes.hpp"
#include "http.hpp"
#include "views.hpp"
#include "flock/automations.hpp"
#include "flock/bot_memory.hpp"
#include "flock/memory_automation.hpp"
#include "flock/json.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace flockv0::web {
namespace {
using httplib::Request;
using httplib::Response;

Id queryId(const Request& req, const char* name) {
    const auto value = req.get_param_value(name);
    Id id = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), id);
    if (req.get_param_value_count(name) != 1 || ec != std::errc{} || end != value.data() + value.size() || id <= 0)
        throw Conflict("Invalid " + std::string(name));
    return id;
}

Json body(const Request& req) {
    const auto data = Json::parse(req.body);
    if (!data.is_object()) throw Conflict("Expected a JSON object");
    return data;
}

template <class F> void handle(const Request& req, Response& res, F run) {
    if (req.body.size() > maxStandardPayload) return error(res, 413, "payload_too_large");
    try {
        run();
    } catch (const AccessDenied&) {
        error(res, 403, "access_denied");
    } catch (const Json::exception&) {
        error(res, 400, "invalid_input");
    } catch (const Conflict& failure) {
        error(res, 409, failure.what());
    }
}

Json requireBot(Store& store, const WebCallbacks& callbacks, const Person& actor, std::string_view bot, Action action) {
    if (!Policy{store}.allows(actor, action, bot)) throw AccessDenied("Bot access denied");
    auto known = callbacks.bots();
    const auto found = std::ranges::find_if(known, [&](const Json& item) { return field(item, "id") == bot; });
    if (found == known.end()) throw Conflict("Bot not found");
    return std::move(*found);
}

Json unscheduledRequests(Store& store, std::string_view bot, const Json& profile) {
    auto requests = Json::array();
    const auto configured = profile.find("cron");
    if (configured == profile.end() || !configured->is_array() || configured->empty()) return requests;

    const auto active = Automations(store).activeConfiguredSchedules(bot);
    for (const auto& entry : *configured) {
        if (!entry.is_object()) continue;
        const auto request = field(entry, "request");
        if (request.empty()) continue;
        const auto saved = active.find(field(entry, "id"));
        if (saved == active.end() || field(*saved, "cron") != field(entry, "schedule") ||
            field(*saved, "timezone") != field(entry, "timezone") || field(*saved, "request") != request)
            requests.push_back(request);
    }
    return Policy{store}.redactSecrets(bot, requests);
}
}

void memoryAutomationRoutes(httplib::Server& server, Store& store, const WebCallbacks& callbacks) {
    server.Get(R"(/api/memory(/history)?)", [&](const Request& req, Response& res) {
        handle(req, res, [&] {
            const auto actor = person(callbacks, req);
            const auto bot = req.get_param_value("bot");
            requireBot(store, callbacks, actor, bot, Action::bot_view);
            Json args = Json::object();
            if (req.has_param("docId")) args["docId"] = req.get_param_value("docId");
            for (auto key : {"before", "revisionId"})
                if (req.has_param(key)) args[key] = queryId(req, key);
            const bool history = req.path.ends_with("/history");
            auto result = Memory(store).memory(actor, bot, history ? "memory_history" : "read_memory", args);
            if (history) projectMemoryHistoryLinks(result);
            reply(res, 200, result);
        });
    });
    server.Post(R"(/api/memory/(write|revert))", [&](const Request& req, Response& res) {
        handle(req, res, [&] {
            const auto args = body(req);
            const auto actor = person(callbacks, req);
            const auto bot = args.at("botId").get<std::string>();
            requireBot(store, callbacks, actor, bot, Action::bot_settings);
            reply(res, 200, Memory(store).memory(actor, bot, req.matches[1] == "write" ? "write_memory" : "revert_memory_edit", args));
        });
    });
    server.Get("/api/schedules", [&](const Request& req, Response& res) {
        handle(req, res, [&] {
            const auto actor = person(callbacks, req);
            const auto bot = req.get_param_value("bot");
            const auto profile = requireBot(store, callbacks, actor, bot, Action::bot_view);
            const auto before = req.has_param("before") ? queryId(req, "before") : 0;
            const bool activeOnly = req.get_param_value("active") == "1";
            auto page = Automations(store).automations(actor, bot, before, activeOnly);
            if (activeOnly && !before) {
                const Policy policy{store};
                page["canManage"] = policy.allows(actor, Action::bot_settings, bot) && policy.allows(actor, Action::bot_decide, bot);
                page["unscheduledRequests"] = unscheduledRequests(store, bot, profile);
                const auto routing = callbacks.intake(bot, "");
                page["needsWorkspace"] = routing.environmentId.empty() || routing.projectId.empty();
            }
            res.set_header("Cache-Control", "no-store");
            reply(res, 200, page);
        });
    });
    server.Post("/api/schedules", [&](const Request& req, Response& res) {
        handle(req, res, [&] {
            const auto args = body(req);
            const auto actor = person(callbacks, req);
            const auto bot = args.at("botId").get<std::string>();
            requireBot(store, callbacks, actor, bot, Action::bot_settings);
            auto routing = callbacks.intake(bot, args.at("request").get<std::string>());
            if (routing.botId != bot) throw Conflict("Bot route changed");
            reply(res, 201, Automations(store).createAutomation(actor, routing, args, callbacks.clock()));
        });
    });
    server.Delete("/api/schedules", [&](const Request& req, Response& res) {
        handle(req, res, [&] {
            const auto actor = person(callbacks, req);
            const auto bot = req.get_param_value("bot");
            requireBot(store, callbacks, actor, bot, Action::bot_settings);
            reply(res, 200, Automations(store).cancelAutomation(actor, bot, queryId(req, "id")));
        });
    });
    server.Post(R"(/api/webhooks/([A-Za-z0-9][A-Za-z0-9_.-]*))", [&](const Request& req, Response& res) {
        handle(req, res, [&] {
            const auto args = body(req);
            auto result =
                Automations(store).receiveWebhook(person(callbacks, req), req.matches[1].str(),
                                                  args.at("externalEventId").get<std::string>(), args.at("event"), callbacks.clock());
            if (result.contains("skippedBots") && result.at("tasks").empty()) {
                result["error"] = "bot_paused";
                return reply(res, 409, result);
            }
            reply(res, 202, result);
        });
    });
}
}
