#include "access_routes.hpp"
#include "flock/memory_automation.hpp"

#include <algorithm>
#include <charconv>

namespace flockv0::web {
namespace {
using httplib::Request;
using httplib::Response;

void reply(Response& res, int status, const Json& data) {
    res.status = status;
    res.set_content(data.dump(),"application/json");
}

Id queryId(const Request& req, const char* name) {
    const auto value = req.get_param_value(name);
    Id id = 0;
    const auto [end,ec] = std::from_chars(value.data(),value.data()+value.size(),id);
    if (req.get_param_value_count(name) != 1 || ec != std::errc{} || end != value.data()+value.size() || id <= 0)
        throw Conflict("Invalid " + std::string(name));
    return id;
}

Json body(const Request& req) {
    const auto data = Json::parse(req.body);
    if (!data.is_object()) throw Conflict("Expected a JSON object");
    return data;
}

template<class F> void handle(const Request& req, Response& res, F run) {
    if (req.body.size() > 128 * 1024) return reply(res,413,{{"error","payload_too_large"}});
    try { run(); }
    catch (const AccessDenied&) { reply(res,403,{{"error","access_denied"}}); }
    catch (const Json::exception&) { reply(res,400,{{"error","invalid_input"}}); }
    catch (const Conflict& error) { reply(res,409,{{"error",error.what()}}); }
}

std::string target(Store& store, const WebCallbacks& callbacks, const Person& actor,
                   std::string bot, Action action) {
    if (!store.policy().allows(actor,action,bot)) throw AccessDenied("Bot access denied");
    const auto known = callbacks.bots();
    if (std::ranges::none_of(known,[&](const Json& item) { return item.value("id",std::string{}) == bot; }))
        throw Conflict("Bot not found");
    return bot;
}
}

void memoryAutomationRoutes(httplib::Server& server, Store& store, const WebCallbacks& callbacks) {
    server.Get(R"(/api/memory(/history)?)",[&](const Request& req, Response& res) {
        handle(req,res,[&] {
            const auto actor = person(callbacks,req);
            const auto bot = target(store,callbacks,actor,req.get_param_value("bot"),Action::bot_view);
            Json args = Json::object();
            if (req.has_param("docId")) args["docId"] = req.get_param_value("docId");
            for (auto key : {"before","proposalsBefore","revisionId","proposalId"})
                if (req.has_param(key)) args[key] = queryId(req,key);
            reply(res,200,store.memory(actor,bot,req.path.ends_with("/history") ? "memory_history" : "read_memory",args));
        });
    });
    server.Post(R"(/api/memory/(propose|apply|revert|reject))",[&](const Request& req, Response& res) {
        handle(req,res,[&] {
            const auto args = body(req);
            const auto actor = person(callbacks,req);
            const auto bot = target(store,callbacks,actor,args.at("botId").get<std::string>(),Action::bot_settings);
            reply(res,200,store.memory(actor,bot,req.matches[1].str()+"_memory_edit",args));
        });
    });
    server.Get("/api/schedules",[&](const Request& req, Response& res) {
        handle(req,res,[&] {
            const auto actor = person(callbacks,req);
            const auto bot = target(store,callbacks,actor,req.get_param_value("bot"),Action::bot_view);
            reply(res,200,store.automations(actor,bot,req.has_param("before") ? queryId(req,"before") : 0));
        });
    });
    server.Post("/api/schedules",[&](const Request& req, Response& res) {
        handle(req,res,[&] {
            const auto args = body(req);
            const auto actor = person(callbacks,req);
            const auto bot = target(store,callbacks,actor,args.at("botId").get<std::string>(),Action::bot_settings);
            auto routing = callbacks.intake(bot,args.at("request").get<std::string>());
            if (routing.botId != bot) throw Conflict("Bot route changed");
            reply(res,201,store.createAutomation(actor,routing,args,callbacks.clock()));
        });
    });
    server.Delete("/api/schedules",[&](const Request& req, Response& res) {
        handle(req,res,[&] {
            const auto actor = person(callbacks,req);
            const auto bot = target(store,callbacks,actor,req.get_param_value("bot"),Action::bot_settings);
            reply(res,200,store.cancelAutomation(actor,bot,queryId(req,"id")));
        });
    });
    server.Post(R"(/api/webhooks/([A-Za-z0-9][A-Za-z0-9_.-]*))",[&](const Request& req, Response& res) {
        handle(req,res,[&] {
            const auto args = body(req);
            const auto result = store.receiveWebhook(person(callbacks,req),req.matches[1].str(),
                args.at("externalEventId").get<std::string>(),args.at("event"),callbacks.clock());
            reply(res,202,result);
        });
    });
}
}
