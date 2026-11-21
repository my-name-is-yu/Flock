#include "flock/web.hpp"
#include "embedded_assets.hpp"
#include "views.hpp"

#include <httplib.h>

#include <charconv>
#include <limits>
#include <thread>

namespace flockv0 {
namespace {
using httplib::Request;
using httplib::Response;
using HandlerResult = httplib::Server::HandlerResponse;
constexpr auto loopback = "127.0.0.1";

void json(Response& res, int status, const Json& body) {
    res.status = status;
    res.set_content(body.dump(-1,' ',false,Json::error_handler_t::replace),"application/json; charset=utf-8");
}

void error(Response& res, int status, std::string_view code) {
    json(res,status,{{"error",code}});
}

struct InputError : std::runtime_error { using std::runtime_error::runtime_error; };

Id cursor(const Request& req) {
    const auto count = req.get_param_value_count("before");
    if (count == 0) return 0;
    if (count != 1) throw InputError("invalid_cursor");
    const auto value = req.get_param_value("before");
    Id id = 0;
    const auto parsed = std::from_chars(value.data(),value.data() + value.size(),id);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || id < 0)
        throw InputError("invalid_cursor");
    return id;
}

Id bodyId(const Json& body, std::string_view key) {
    const auto value = body.find(key);
    if (value == body.end() || !value->is_number_integer() ||
        (value->is_number_unsigned() && value->get<std::uint64_t>() > std::numeric_limits<Id>::max()))
        throw InputError("invalid_id");
    const auto id = value->get<Id>();
    if (id <= 0) throw InputError("invalid_id");
    return id;
}

std::string requiredText(const Json& body, std::string_view key, std::size_t bytes) {
    auto text = web::field(body,key);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos || text.size() > bytes || text.find('\0') != std::string::npos)
        throw InputError("invalid_text");
    return text;
}

Json operationData(const Json& body, std::string_view operation) {
    if (operation == "answer") return {{"text",requiredText(body,"text",8000)}};
    if (operation == "extend") {
        const auto deadline = bodyId(body,"deadline");
        if (deadline <= unixTime()) throw InputError("invalid_deadline");
        return {{"deadline",deadline}};
    }
    if (operation == "reconcile") {
        Json data = Json::object();
        for (const auto* key : {"threadId","runId","questionId"}) {
            if (!body.contains(key)) continue;
            data[key] = requiredText(body,key,1024);
        }
        if (data.contains("threadId") != data.contains("runId") ||
            (data.contains("questionId") && !data.contains("runId"))) throw InputError("invalid_reconciliation_target");
        return data;
    }
    if (operation != "approve" && operation != "retry" && operation != "cancel")
        throw InputError("invalid_operation");
    return Json::object();
}

class LoopbackServer final : public WebServer {
public:
    LoopbackServer(Store& store, unsigned port, WebCallbacks callbacks)
        : store_(store), callbacks_(std::move(callbacks)) {
        if (port > 65535 || !callbacks_.bots || !callbacks_.intake)
            throw std::invalid_argument("Invalid Flock Web listener settings");
        server_.new_task_queue = [] { return new httplib::ThreadPool(4,8,32); };
        server_.set_address_family(AF_INET)
            .set_payload_max_length(128 * 1024)
            .set_read_timeout(3)
            .set_write_timeout(3)
            .set_keep_alive_timeout(1)
            .set_default_headers({
                {"Cache-Control","no-store"},
                {"Content-Security-Policy","default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
                   "img-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'"},
                {"Cross-Origin-Resource-Policy","same-origin"},
                {"Referrer-Policy","no-referrer"},
                {"X-Content-Type-Options","nosniff"},
                {"X-Frame-Options","DENY"}
            });
        server_.set_pre_routing_handler([this](const Request& req, Response& res) { return guard(req,res); });
        server_.set_exception_handler([](const Request&, Response& res, std::exception_ptr) {
            error(res,500,"server_error");
        });
        server_.set_error_handler([](const Request&, Response& res) {
            if (res.body.empty()) error(res,res.status,"http_error");
        });
        for (const auto& asset : web::assets) {
            server_.Get(asset.path == "/index.html" ? "/" : std::string(asset.path),
                [asset](const Request&, Response& res) {
                    res.set_content(asset.body.data(),asset.body.size(),std::string(asset.type));
                });
        }
        server_.Get("/api/bots",[this](const Request&, Response& res) {
            json(res,200,web::botList(store_,bots()));
        });
        for (const auto path : {"/api/view","/api/conversation","/api/sessions"}) {
            server_.Get(path,[this](const Request& req, Response& res) {
                const auto id = req.get_param_value("bot");
                if (req.get_param_value_count("bot") != 1 || id.empty()) return error(res,400,"invalid_bot");
                const auto bot = findBot(id);
                if (bot.is_null()) return error(res,404,"bot_not_found");
                try {
                    const auto before = cursor(req);
                    if (req.path == "/api/view") json(res,200,web::botView(store_,bot));
                    else if (req.path == "/api/conversation") json(res,200,web::conversation(store_,id,before));
                    else json(res,200,web::sessions(store_,id,before));
                } catch (const InputError& e) { error(res,400,e.what()); }
            });
        }
        server_.Post("/api/requests",[this](const Request& req, Response& res) { mutate(req,res,true); });
        server_.Post("/api/operations",[this](const Request& req, Response& res) { mutate(req,res,false); });

        const auto bound = port ? (server_.bind_to_port(loopback,static_cast<int>(port)) ? static_cast<int>(port) : -1)
                                : server_.bind_to_any_port(loopback);
        if (bound <= 0) throw std::runtime_error("Could not bind the Flock Web loopback listener");
        port_ = static_cast<unsigned>(bound);
        try {
            worker_ = std::thread([this] { server_.listen_after_bind(); });
        } catch (...) {
            server_.stop();
            throw;
        }
        server_.wait_until_ready();
        if (!server_.is_running()) {
            worker_.join();
            throw std::runtime_error("Could not start the Flock Web loopback listener");
        }
    }

    ~LoopbackServer() override {
        server_.stop();
        worker_.join();
    }
    unsigned port() const override { return port_; }

private:
    HandlerResult guard(const Request& req, Response& res) const {
        const auto portSuffix = port_ == 80 ? std::string{} : ":" + std::to_string(port_);
        const auto host = req.get_header_value("Host");
        if (req.get_header_value_count("Host") != 1 ||
            (host != loopback + portSuffix && host != "localhost" + portSuffix)) {
            error(res,403,"invalid_host");
            return HandlerResult::Handled;
        }
        const auto origin = req.get_header_value("Origin");
        const auto origins = req.get_header_value_count("Origin");
        const auto fetchSite = req.get_header_value("Sec-Fetch-Site");
        const bool sameOrigin = origins == 1 && origin == "http://" + host;
        if ((origins != 0 && !sameOrigin) || (req.has_header("Sec-Fetch-Site") &&
            (req.get_header_value_count("Sec-Fetch-Site") != 1 || (fetchSite != "same-origin" && fetchSite != "none")))) {
            error(res,403,"invalid_origin");
            return HandlerResult::Handled;
        }
        if (req.method == "GET" || req.method == "HEAD") return HandlerResult::Unhandled;
        if (req.method != "POST") error(res,405,"method_not_allowed");
        else if (!sameOrigin || req.get_header_value_count("X-Flock-Action") != 1 ||
                 req.get_header_value("X-Flock-Action") != "1") error(res,403,"invalid_origin");
        else if (req.get_header_value_count("Content-Type") != 1 ||
                 req.get_header_value("Content-Type") != "application/json") error(res,415,"expected_json");
        else return HandlerResult::Unhandled;
        return HandlerResult::Handled;
    }

    Json bots() const {
        auto list = callbacks_.bots();
        if (!list.is_array()) throw std::runtime_error("Bot descriptors must be an array");
        return list;
    }

    Json findBot(std::string_view id) const {
        for (auto& bot : bots())
            if (web::field(bot,"id") == id) return std::move(bot);
        return nullptr;
    }

    void mutate(const Request& req, Response& res, bool submit) {
        const auto body = Json::parse(req.body,nullptr,false);
        if (!body.is_object()) return error(res,400,"invalid_json");
        try {
            const auto bot = requiredText(body,"botId",1024);
            if (findBot(bot).is_null()) return error(res,404,"bot_not_found");
            if (submit) {
                const auto request = requiredText(body,"request",16000);
                Intake intake;
                try { intake = callbacks_.intake(bot,request); }
                catch (const std::exception&) { return error(res,422,"intake_settings"); }
                // The caller supplies only a Bot and text. Routing and config come from the callback.
                intake.botId = bot;
                intake.request = request;
                intake.scheduleId.clear();
                intake.scheduledAt = 0;
                return json(res,201,{{"taskId",store_.submit(intake)}});
            }
            const auto id = bodyId(body,"taskId");
            const auto wait = bodyId(body,"waitId");
            const auto operation = requiredText(body,"operation",32);
            const auto data = operationData(body,operation);
            if (web::field(store_.task(id),"botId") != bot) return error(res,404,"task_not_found");
            store_.operate(id,wait,operation,data);
            json(res,200,{{"accepted",true}});
        } catch (const InputError& e) { error(res,400,e.what()); }
        catch (const Conflict&) { error(res,409,submit ? "request_conflict" : "operation_conflict"); }
    }

    Store& store_;
    WebCallbacks callbacks_;
    httplib::Server server_;
    std::thread worker_;
    unsigned port_ = 0;
};
} // namespace

std::unique_ptr<WebServer> startWeb(Store& store, unsigned port, WebCallbacks callbacks) {
    return std::make_unique<LoopbackServer>(store,port,std::move(callbacks));
}
} // namespace flockv0
