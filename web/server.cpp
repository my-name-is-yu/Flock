#include "flock/web.hpp"
#include "change_feed.hpp"
#include "embedded_assets.hpp"
#include "views.hpp"
#include "access_routes.hpp"
#include "board_inbox_routes.hpp"
#include "git_routes.hpp"
#include "flock/board_inbox.hpp"
#include "channel_routes.hpp"
#include "session_handoff_routes.hpp"

#include <httplib.h>

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <thread>

namespace flockv0 {
namespace {
using web::ChangeFeed;
using httplib::Request;
using httplib::Response;
using HandlerResult = httplib::Server::HandlerResponse;
constexpr auto loopback = "127.0.0.1";
constexpr std::size_t maxStandardPayload = 128 * 1024;
constexpr std::size_t maxProfilePayload = 7 * 1024 * 1024 + 128 * 1024;
enum class Mutation { Message, Request, Operation };

void json(Response& res, int status, const Json& body) {
    res.status = status;
    res.set_content(body.dump(-1,' ',false,Json::error_handler_t::replace),"application/json; charset=utf-8");
}

void error(Response& res, int status, std::string_view code) {
    json(res,status,{{"error",code}});
}

void setViewer(Response& res, const WebCallbacks& callbacks, const Person& person) {
    const Json viewer = callbacks.authenticate ? Json(person.id) : Json(nullptr);
    res.set_header("X-Flock-Viewer",viewer.dump(-1,' ',true,Json::error_handler_t::replace));
}

std::string_view verifiedViewer(const WebCallbacks& callbacks, const Person& person) {
    // Local access shares one principal, so it cannot identify a human author.
    return callbacks.authenticate && person.id != localOwner.id ? std::string_view{person.id} : std::string_view{};
}

struct InputError : std::runtime_error { using std::runtime_error::runtime_error; };

Id queryId(const Request& req, const char* key, bool optional) {
    const auto error = optional ? "invalid_cursor" : "invalid_id";
    const auto count = req.get_param_value_count(key);
    if (count == 0 && optional) return 0;
    if (count != 1) throw InputError(error);
    const auto value = req.get_param_value(key);
    Id id = 0;
    const auto parsed = std::from_chars(value.data(),value.data() + value.size(),id);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || id < (optional ? 0 : 1))
        throw InputError(error);
    return id;
}

Id bodyId(const Json& body, std::string_view key, Id minimum = 1) {
    const auto value = body.find(key);
    if (value == body.end() || !value->is_number_integer() ||
        (value->is_number_unsigned() && value->get<std::uint64_t>() > std::numeric_limits<Id>::max()))
        throw InputError("invalid_id");
    const auto id = value->get<Id>();
    if (id < minimum) throw InputError("invalid_id");
    return id;
}

std::string requiredText(const Json& body, std::string_view key, std::size_t bytes) {
    auto text = web::field(body,key);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos || text.size() > bytes || text.find('\0') != std::string::npos)
        throw InputError("invalid_text");
    return text;
}

const Json* botById(const Json& bots, std::string_view id) {
    const auto bot = std::ranges::find_if(bots,[&](const Json& descriptor) { return web::field(descriptor,"id") == id; });
    return bot == bots.end() ? nullptr : &*bot;
}

int base64Digit(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

std::string decodeBase64(std::string_view input, std::size_t maxBytes) {
    if (input.empty() || input.size() % 4 != 0) throw InputError("invalid_image");
    std::size_t padding = 0;
    if (input.back() == '=') {
        ++padding;
        if (input[input.size() - 2] == '=') ++padding;
    }
    const std::size_t decodedSize = (input.size() / 4) * 3 - padding;
    if (decodedSize == 0 || decodedSize > maxBytes) throw InputError("invalid_image");
    std::string out;
    out.reserve(decodedSize);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const int a = base64Digit(static_cast<unsigned char>(input[i]));
        const int b = base64Digit(static_cast<unsigned char>(input[i + 1]));
        const bool padC = input[i + 2] == '=';
        const bool padD = input[i + 3] == '=';
        if (a < 0 || b < 0 || (padC && !padD)) throw InputError("invalid_image");
        if ((padC || padD) && i + 4 != input.size()) throw InputError("invalid_image");
        const int c = padC ? 0 : base64Digit(static_cast<unsigned char>(input[i + 2]));
        const int d = padD ? 0 : base64Digit(static_cast<unsigned char>(input[i + 3]));
        if (c < 0 || d < 0) throw InputError("invalid_image");
        out.push_back(static_cast<char>((a << 2) | (b >> 4)));
        if (!padC) out.push_back(static_cast<char>(((b & 0x0F) << 4) | (c >> 2)));
        if (!padD) out.push_back(static_cast<char>(((c & 0x03) << 6) | d));
    }
    return out;
}

struct ParsedProfile {
    Json profile;
    std::optional<AvatarImage> image;
};

ParsedProfile parseBotProfile(const Json& body, bool creating) {
    Json profile = {{"name",validatedBotName(body)}};
    if (creating && body.contains("role")) {
        if (!body["role"].is_string() || !validBotRole(body["role"].get_ref<const std::string&>()))
            throw InputError("invalid_role");
        profile["role"] = body["role"];
    }
    // Updates may omit the avatar to rename without touching the stored appearance.
    if (!creating && !body.contains("avatar")) return {std::move(profile),std::nullopt};
    auto avatar = validatedAvatarStyle(body);
    std::optional<AvatarImage> image;
    if (avatar["kind"] == "upload") {
        const auto& source = body.at("avatar");
        if (source.contains("filename")) {
            if (!source["filename"].is_string() || source["filename"].get_ref<const std::string&>().size() > 256)
                throw InputError("invalid_avatar");
            avatar["filename"] = source["filename"];
        }
        if (source.contains("imageData") && !source["imageData"].is_null()) {
            if (!source["imageData"].is_string()) throw InputError("invalid_image");
            image.emplace(decodeBase64(source["imageData"].get_ref<const std::string&>(),AvatarImage::maxBytes));
        } else if (creating) {
            throw InputError("invalid_image");
        }
    }
    profile["avatar"] = std::move(avatar);
    return {std::move(profile),std::move(image)};
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

bool localApprovalNavigation(const Request& req) {
    if (req.method != "GET" || req.has_header("Origin") ||
        req.get_header_value("Sec-Fetch-Site") != "cross-site" ||
        req.get_header_value("Sec-Fetch-Mode") != "navigate" ||
        req.get_header_value("Sec-Fetch-Dest") != "document") return false;
    std::string_view path = req.path;
    constexpr std::string_view prefix = "/settings/", suffix = "/access";
    if (!path.starts_with(prefix)) return false;
    path.remove_prefix(prefix.size());
    return path.ends_with(suffix) && validBotId(path.substr(0,path.size()-suffix.size()));
}

class LoopbackServer final : public WebServer {
public:
    LoopbackServer(Store& store, unsigned port, WebCallbacks callbacks)
        : store_(store), callbacks_(std::move(callbacks)),
          changes_(store,[this](const ChangeFeed::Scope& scope) {
              return std::visit([&](const auto& target) { return snapshot(scope.person,target); },scope.target);
          }) {
        if (port > 65535 || !callbacks_.bots || !callbacks_.intake)
            throw std::invalid_argument("Invalid Flock Web listener settings");
        if (callbacks_.authenticate) store_.access().requireWebIdentity();
        store_.protectGitCredential(callbacks_.githubWebhookSecret);
        server_.new_task_queue = [] { return new httplib::ThreadPool(4,8,32); };
        server_.set_address_family(AF_INET)
            .set_payload_max_length(maxProfilePayload)
            .set_read_timeout(10)
            .set_write_timeout(10)
            .set_keep_alive_timeout(1)
            .set_default_headers({
                {"Cache-Control","no-store"},
                {"Content-Security-Policy","default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
                   "img-src 'self' blob:; base-uri 'none'; frame-ancestors 'none'; form-action 'none'"},
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
        server_.Get(R"(/settings/([^/]+)/(secrets|environment|access))",[](const Request&, Response& res) {
            for (const auto& asset : web::assets) {
                if (asset.path == "/access.html") {
                    res.set_content(asset.body.data(),asset.body.size(),std::string(asset.type));
                    return;
                }
            }
            error(res,404,"settings_not_found");
        });
        web::accessRoutes(server_,store_,callbacks_);
        web::memoryAutomationRoutes(server_,store_,callbacks_);
        web::boardInboxRoutes(server_,store_,callbacks_);
        web::gitRoutes(server_,store_,callbacks_);
        web::channelRoutes(server_,store_,callbacks_);
        web::sessionHandoffRoutes(server_,callbacks_);
        server_.Get("/api/bots",[this](const Request& req, Response& res) {
            const auto actor = web::person(callbacks_,req);
            setViewer(res,callbacks_,actor);
            json(res,200,botList(actor));
        });
        server_.Get("/api/events",[this](const Request& req, Response& res) { changes(req,res); });
        server_.Get("/api/bot-avatar",[this](const Request& req, Response& res) {
            const auto id = req.get_param_value("bot");
            if (req.get_param_value_count("bot") != 1 || id.empty()) return error(res,400,"invalid_bot");
            if (!store_.access().permitsPerson(web::person(callbacks_,req),id,Role::view)) return error(res,403,"access_denied");
            if (!callbacks_.botAvatar) return error(res,404,"avatar_not_found");
            const auto image = callbacks_.botAvatar(id);
            if (!image) return error(res,404,"avatar_not_found");
            res.set_content(image->bytes().data(),image->bytes().size(),std::string(image->format().contentType));
        });
        for (const auto path : {"/api/view","/api/workflows","/api/compactions","/api/conversation","/api/peer-conversation","/api/sessions","/api/task"}) {
            server_.Get(path,[this](const Request& req, Response& res) {
                const auto id = req.get_param_value("bot");
                if (req.get_param_value_count("bot") != 1 || id.empty()) return error(res,400,"invalid_bot");
                const auto actor = web::person(callbacks_,req);
                if (!store_.access().permitsPerson(actor,id,Role::view)) return error(res,403,"access_denied");
                const auto known = bots();
                const auto* bot = botById(known,id);
                if (!bot) return error(res,404,"bot_not_found");
                try {
                    if (req.path == "/api/task") {
                        const auto taskId = queryId(req,"taskId",false);
                        const auto task = store_.task(taskId);
                        if (web::field(task,"botId") != id) return error(res,404,"task_not_found");
                        return json(res,200,store_.access().redactSecrets(id,web::taskDetail(task,store_.latestTaskResult(taskId))));
                    }
                    const auto before = queryId(req,"before",true);
                    if (req.path == "/api/peer-conversation") return readPeerConversation(req,res,known,actor,id,before);
                    if (req.path == "/api/compactions" || req.path == "/api/workflows") {
                        const auto items = req.path == "/api/compactions" ? store_.compactions(id,before) : store_.workflows(id,before);
                        return json(res,200,store_.access().redactSecrets(id,items));
                    }
                    if (req.path == "/api/view") {
                        const auto changesAfter = req.has_param("changesAfter")
                            ? std::optional{queryId(req,"changesAfter",true)} : std::nullopt;
                        const auto sessionsAfter = req.has_param("sessionsAfter")
                            ? std::optional{queryId(req,"sessionsAfter",true)} : std::nullopt;
                        const auto changes = store_.conversationChangesSince(changesAfter,id);
                        const auto [sessionThrough,sessionTasks] = store_.sessionChangesSince(id,sessionsAfter);
                        auto view = store_.access().redactSecrets(id,web::botView(store_,*bot,sessionsAfter ? &sessionTasks : nullptr,actor.id));
                        view["bot"]["id"] = id;
                        view["composerPerson"] = callbacks_.authenticate ? Json(actor.id) : Json(nullptr);
                        view["historyCursor"] = changes.through;
                        view["changedMessageIds"] = Json::array();
                        if (const auto changed = changes.messages.find(id); changed != changes.messages.end())
                            view["changedMessageIds"] = changed->second;
                        view["sessionCursor"] = sessionThrough;
                        setViewer(res,callbacks_,actor);
                        json(res,200,withConversationAccess(actor,id,std::move(view)));
                    } else if (req.path == "/api/conversation") json(res,200,store_.access().redactSecrets(id,web::conversation(store_,id,before,actor.id)));
                    else json(res,200,store_.access().redactSecrets(id,web::sessions(store_,id,before)));
                } catch (const InputError& e) { error(res,400,e.what()); }
                catch (const Conflict&) { error(res,404,"task_not_found"); }
            });
        }
        server_.Post("/api/bots",[this](const Request& req, Response& res) { saveBot(req,res,false); });
        server_.Post("/api/bots/profile",[this](const Request& req, Response& res) { saveBot(req,res,true); });
        server_.Post("/api/messages",[this](const Request& req, Response& res) { mutate(req,res,Mutation::Message); });
        server_.Post("/api/requests",[this](const Request& req, Response& res) { mutate(req,res,Mutation::Request); });
        server_.Post("/api/operations",[this](const Request& req, Response& res) { mutate(req,res,Mutation::Operation); });

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
        changes_.stop();
        server_.stop();
        worker_.join();
        store_.setWebOrigin({});
    }
    unsigned port() const override { return port_; }

private:
    void changes(const Request& req, Response& res) {
        const auto bot = req.get_param_value("bot");
        const auto peer = req.get_param_value("peer");
        if (req.get_param_value_count("bot") > 1 || req.get_param_value_count("peer") > 1 ||
            bot.size() > 1024 || peer.size() > 1024 ||
            (req.has_param("bot") && bot.empty()) ||
            (req.has_param("peer") && (bot.empty() || peer.empty() || bot == peer)))
            return error(res,400,"invalid_subscription");
        const auto person = web::person(callbacks_,req);
        auto access = store_.access();
        if ((!bot.empty() && !access.permitsPerson(person,bot,Role::view)) ||
            (!peer.empty() && !access.permitsPerson(person,peer,Role::view)))
            return error(res,403,"access_denied");
        ChangeFeed::Target target = ChangeFeed::BotList{};
        if (!peer.empty()) target = ChangeFeed::DirectMessage{bot,peer};
        else if (!bot.empty()) target = ChangeFeed::BotView{bot};
        auto subscription = changes_.subscribe({person,std::move(target)});
        if (!subscription) return error(res,503,"event_stream_unavailable");
        auto cursor = std::make_shared<web::ChangeFeed::Cursor>(std::move(*subscription));
        res.set_chunked_content_provider("text/event-stream",
            [this,cursor](std::size_t offset, httplib::DataSink& sink) {
                if (!sink.is_writable()) return false;
                if (offset == 0) {
                    constexpr std::string_view ready = "retry: 2500\nevent: ready\ndata: {}\n\n";
                    return sink.write(ready.data(),ready.size());
                }
                const auto update = changes_.next(*cursor);
                if (!update) {
                    sink.done();
                    return true;
                }
                if (!sink.is_writable()) return false;
                const auto event = update->empty()
                    ? std::string("event: ping\ndata: {}\n\n")
                    : "event: change\ndata: " + update->dump(-1,' ',false,Json::error_handler_t::replace) + "\n\n";
                return sink.write(event.data(),event.size());
            },
            [this,cursor](bool) { changes_.release(*cursor); });
    }

    HandlerResult guard(const Request& req, Response& res) const {
        const auto portSuffix = port_ == 80 ? std::string{} : ":" + std::to_string(port_);
        const auto host = req.get_header_value("Host");
        if (req.get_header_value_count("Host") != 1 ||
            (host != loopback + portSuffix && host != "localhost" + portSuffix)) {
            error(res,403,"invalid_host");
            return HandlerResult::Handled;
        }
        if ((req.method == "GET" || req.method == "HEAD") && web::artifactPreview(req.path))
            return HandlerResult::Unhandled;
        const auto origin = req.get_header_value("Origin");
        const auto origins = req.get_header_value_count("Origin");
        const auto fetchSite = req.get_header_value("Sec-Fetch-Site");
        const bool sameOrigin = origins == 1 && origin == "http://" + host;
        if ((origins != 0 && !sameOrigin) || (req.has_header("Sec-Fetch-Site") &&
            (req.get_header_value_count("Sec-Fetch-Site") != 1 ||
             (fetchSite != "same-origin" && fetchSite != "none" && !localApprovalNavigation(req))))) {
            error(res,403,"invalid_origin");
            return HandlerResult::Handled;
        }
        // The MCP tool uses its own short-lived pairing credential. Browser identity
        // and CSRF headers do not authorize a call to this endpoint.
        if (req.path == "/mcp/flock") {
            if (origins || req.has_header("Sec-Fetch-Site")) error(res,403,"invalid_origin");
            else if (req.method != "POST") {
                res.set_header("Allow","POST");
                error(res,405,"method_not_allowed");
            }
            else return HandlerResult::Unhandled;
            return HandlerResult::Handled;
        }
        if (!origins && !req.has_header("Sec-Fetch-Site") && req.method == "POST" && req.path.starts_with("/api/git/webhooks/")) return HandlerResult::Unhandled;
        if (req.method == "POST" && web::bridgeEndpoint(req.path)) {
            if (origins || req.has_header("Sec-Fetch-Site")) error(res,403,"invalid_origin");
            else if (req.get_header_value_count("Content-Type") != 1 ||
                     req.get_header_value("Content-Type") != "application/json") error(res,415,"expected_json");
            else return HandlerResult::Unhandled;
            return HandlerResult::Handled;
        }
        if (web::person(callbacks_,req).id.empty() || (!callbacks_.authenticate && store_.access().requiresIdentity())) {
            res.set_header("WWW-Authenticate","Basic realm=\"Flock\"");
            error(res,401,"identity_required");
            return HandlerResult::Handled;
        }
        if (req.method == "GET" || req.method == "HEAD") return HandlerResult::Unhandled;
        const bool deleting = req.method == "DELETE" && req.path == "/api/schedules";
        const bool machine = req.path.starts_with("/api/webhooks/") && origins == 0 && !req.has_header("Sec-Fetch-Site");
        if (req.method != "POST" && !deleting) error(res,405,"method_not_allowed");
        else if ((!sameOrigin && !machine) || req.get_header_value_count("X-Flock-Action") != 1 ||
                 req.get_header_value("X-Flock-Action") != "1") error(res,403,"invalid_origin");
        else if (deleting) return HandlerResult::Unhandled;
        else if (req.get_header_value_count("Content-Type") != 1 ||
                 req.get_header_value("Content-Type") != "application/json") error(res,415,"expected_json");
        else if (req.path != "/api/bots" && req.path != "/api/bots/profile") {
            const auto lengthHeader = req.get_header_value("Content-Length");
            std::size_t contentLength = 0;
            if (!lengthHeader.empty()) {
                const auto parsed = std::from_chars(lengthHeader.data(),lengthHeader.data() + lengthHeader.size(),contentLength);
                if (parsed.ec == std::errc{} && parsed.ptr == lengthHeader.data() + lengthHeader.size() &&
                    contentLength > maxStandardPayload) {
                    error(res,413,"payload_too_large");
                    return HandlerResult::Handled;
                }
            }
            return HandlerResult::Unhandled;
        }
        else return HandlerResult::Unhandled;
        return HandlerResult::Handled;
    }

    void saveBot(const Request& req, Response& res, bool update) {
        if (update ? !callbacks_.updateBot : !callbacks_.createBot)
            return error(res,501,"bot_management_unavailable");
        const auto body = Json::parse(req.body,nullptr,false);
        if (!body.is_object()) return error(res,400,"invalid_json");
        try {
            if (update) {
                const auto botId = requiredText(body,"botId",1024);
                if (!store_.access().permitsPerson(web::person(callbacks_,req),botId,Role::admin)) return error(res,403,"access_denied");
                if (findBot(botId).is_null()) return error(res,404,"bot_not_found");
                auto parsed = parseBotProfile(body,false);
                if (store_.access().redactSecrets(botId,parsed.profile) != parsed.profile)
                    return error(res,400,"invalid_bot_profile");
                const auto saved = callbacks_.updateBot(botId,parsed.profile,parsed.image);
                changes_.rescan();
                return json(res,200,redactBot(botId,web::botSettings(saved)));
            }
            if (!web::person(callbacks_,req).adminRoot) return error(res,403,"access_denied");
            auto parsed = parseBotProfile(body,true);
            const auto created = callbacks_.createBot(parsed.profile,parsed.image);
            changes_.rescan();
            return json(res,201,redactBot(web::field(created,"id"),web::botSettings(created)));
        } catch (const InputError& e) { error(res,400,e.what()); }
        catch (const BotProfileError& e) { error(res,400,e.code); }
        catch (const Conflict&) { error(res,400,"invalid_bot_profile"); }
    }

    Json bots() const {
        auto list = callbacks_.bots();
        if (!list.is_array()) throw std::runtime_error("Bot descriptors must be an array");
        return list;
    }

    Json redactBot(std::string_view id, const Json& view) const {
        auto bot = store_.access().redactSecrets(id,view);
        bot["id"] = id;
        return bot;
    }

    Json findBot(std::string_view id) const {
        for (auto& bot : bots())
            if (web::field(bot,"id") == id) return std::move(bot);
        return nullptr;
    }

    std::vector<PeerBot> peers(std::string_view bot) const {
        return callbacks_.peers ? callbacks_.peers(bot) : std::vector<PeerBot>{};
    }

    bool grantsPeer(std::string_view bot, std::string_view peer) const {
        return std::ranges::any_of(peers(bot),[&](const PeerBot& entry) { return entry.intake.botId == peer; });
    }

    bool canReadPeer(std::string_view bot, std::string_view peer, bool configured) const {
        if (configured && (grantsPeer(bot,peer) || grantsPeer(peer,bot))) return true;
        return store_.hasPeerExchange(bot,peer);
    }

    Json snapshot(const Person& person, const ChangeFeed::BotList&) const {
        return {{"bots",botList(person)},{"inbox",store_.inbox(person,{})}};
    }

    Json withConversationAccess(const Person& actor, std::string_view bot, Json view) const {
        view["canDecide"] = store_.access().permitsPerson(actor,bot,Role::decide);
        return view;
    }

    Json snapshot(const Person& person, const ChangeFeed::BotView& view) const {
        if (!store_.access().permitsPerson(person,view.bot,Role::view)) return nullptr;
        const auto bot = findBot(view.bot);
        if (bot.is_null()) return nullptr;
        return withConversationAccess(person,view.bot,store_.access().redactSecrets(view.bot,web::botView(store_,bot,nullptr,person.id)));
    }

    Json snapshot(const Person& person, const ChangeFeed::DirectMessage& dm) const {
        auto access = store_.access();
        if (!access.permitsPerson(person,dm.bot,Role::view) ||
            !access.permitsPerson(person,dm.peer,Role::view)) return nullptr;
        const auto known = bots();
        if (!botById(known,dm.bot) || !canReadPeer(dm.bot,dm.peer,botById(known,dm.peer) != nullptr))
            return nullptr;
        return access.redactSecrets(dm.peer,access.redactSecrets(dm.bot,web::peerConversation(store_,dm.bot,dm.peer,0)));
    }

    Json botList(const Person& person) const {
        auto visible = bots();
        auto access = store_.access();
        visible.erase(std::remove_if(visible.begin(),visible.end(),[&](const Json& bot) {
            return !access.permitsPerson(person,web::field(bot,"id"),Role::view);
        }),visible.end());
        auto list = web::botList(store_,visible);
        if (list.empty()) return list;
        std::set<std::string> known;
        for (const auto& bot : list) known.insert(web::field(bot,"id"));
        auto pairs = store_.peerExchangePairs();
        for (const auto& id : known) {
            for (const auto& entry : peers(id)) {
                const auto& peer = entry.intake.botId;
                if (known.contains(peer)) pairs.emplace_back(id,peer);
            }
        }
        std::map<std::string,std::set<std::string>> connections;
        for (const auto& [sender,recipient] : pairs) {
            if (sender == recipient) continue;
            if (known.contains(sender) && access.permitsPerson(person,recipient,Role::view)) connections[sender].insert(recipient);
            if (known.contains(recipient) && access.permitsPerson(person,sender,Role::view)) connections[recipient].insert(sender);
        }
        for (auto& bot : list) {
            const auto id = web::field(bot,"id");
            bot = redactBot(id,bot);
            auto& entries = bot["peers"] = Json::array();
            for (const auto& peer : connections[id]) entries.push_back({{"id",peer}});
        }
        return list;
    }

    void readPeerConversation(const Request& req, Response& res, const Json& known, const Person& actor,
                              std::string_view bot, Id before) {
        const auto peer = req.get_param_value("peer");
        if (req.get_param_value_count("peer") != 1 || peer.empty() || bot == peer)
            return error(res,400,"invalid_peer");
        if (!store_.access().permitsPerson(actor,peer,Role::view))
            return error(res,403,"access_denied");
        const bool configured = botById(known,peer) != nullptr;
        if (!canReadPeer(bot,peer,configured))
            return configured ? error(res,403,"peer_not_allowed") : error(res,404,"peer_not_found");
        Id anchor = 0;
        if (req.has_param("anchor")) {
            if (req.has_param("before")) return error(res,400,"invalid_cursor");
            anchor = store_.peerMessageAnchor(bot,peer,queryId(req,"anchor",false));
            if (!anchor) return error(res,404,"peer_message_not_found");
        }
        const auto history = store_.access().redactSecrets(bot,
            web::peerConversation(store_,bot,peer,before,anchor,verifiedViewer(callbacks_,actor)));
        json(res,200,store_.access().redactSecrets(peer,history));
    }

    void mutate(const Request& req, Response& res, Mutation mutation) {
        if (req.body.size() > maxStandardPayload) return error(res,413,"payload_too_large");
        const auto body = Json::parse(req.body,nullptr,false);
        if (!body.is_object()) return error(res,400,"invalid_json");
        try {
            const auto bot = requiredText(body,"botId",1024);
            const auto actor = web::person(callbacks_,req);
            if (!store_.access().permitsPerson(actor,bot,Role::decide)) return error(res,403,"access_denied");
            if (!botById(bots(),bot)) return error(res,404,"bot_not_found");
            if (mutation != Mutation::Operation) {
                const auto message = mutation == Mutation::Message;
                const auto request = requiredText(body,message ? "text" : "request",16000);
                const auto clientMessageId = message ? requiredText(body,"clientMessageId",256) : std::string{};
                const auto replyTo = message && body.contains("replyToMessageId") ? bodyId(body,"replyToMessageId") : Id{};
                const auto attachments = body.value("attachments",Json::array());
                if (message) {
                    const auto previous = store_.receipt(actor,bot,clientMessageId,request,replyTo,attachments);
                    if (previous) return json(res,201,*previous);
                }
                Intake intake;
                try { intake = callbacks_.intake(bot,request); }
                catch (const std::exception&) {
                    if (!message) return error(res,422,"intake_settings");
                }
                // The caller supplies only a Bot and text. Routing and config come from the callback.
                intake.botId = bot;
                intake.request = request;
                if (message) return json(res,201,store_.receive(actor,intake,clientMessageId,request,replyTo,attachments));
                return json(res,201,{{"taskId",store_.submit(intake)}});
            }
            const auto operation = requiredText(body,"operation",32);
            const auto id = bodyId(body,"taskId");
            const auto wait = bodyId(body,"waitId");
            auto data = operationData(body,operation);
            const auto task = store_.task(id);
            if (web::field(task,"botId") != bot) return error(res,404,"task_not_found");
            if (operation == "reconcile") data = reconciliationData(task,wait,std::move(data),callbacks_.intake);
            store_.operate(actor,{bot,id,wait,bodyId(body,"targetRevision",0),bodyId(body,"targetGeneration",0)},operation,data);
            json(res,200,{{"accepted",true}});
        } catch (const InputError& e) { error(res,400,e.what()); }
        catch (const DecisionDenied&) { error(res,403,"access_denied"); }
        catch (const RequestChanged&) { error(res,409,"request_changed"); }
        catch (const Conflict&) {
            const auto code = mutation == Mutation::Message ? "message_conflict" :
                mutation == Mutation::Request ? "request_conflict" : "operation_conflict";
            error(res,409,code);
        }
    }

    Store& store_;
    WebCallbacks callbacks_;
    web::ChangeFeed changes_;
    httplib::Server server_;
    std::thread worker_;
    unsigned port_ = 0;
};
} // namespace

std::unique_ptr<WebServer> startWeb(Store& store, unsigned port, WebCallbacks callbacks) {
    auto server = std::make_unique<LoopbackServer>(store,port,std::move(callbacks));
    store.setWebOrigin("http://" + std::string(loopback) + ":" + std::to_string(server->port()));
    return server;
}
} // namespace flockv0
