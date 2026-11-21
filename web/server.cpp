#include "flock/web.hpp"
#include "change_feed.hpp"
#include "embedded_assets.hpp"
#include "views.hpp"

#include <httplib.h>

#include <algorithm>
#include <array>
#include <cctype>
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
constexpr std::size_t maxAvatarImageBytes = 5 * 1024 * 1024;
constexpr std::size_t maxProfilePayload = 7 * 1024 * 1024 + 128 * 1024;
constexpr std::array<std::string_view,7> allowedShapes = {
    "mug", "sprout", "controller", "lightbulb", "chat", "watering-can", "barbell"
};
constexpr std::array<std::string_view,8> allowedIcons = {
    "star", "orbit", "zap", "compass", "feather", "hexagon", "telescope", "sparkles"
};
constexpr std::array<std::string_view,4> allowedRoles = {
    "reviewer", "builder", "researcher", "custom"
};
constexpr std::array<std::string_view,4> allowedImageTypes = {
    "image/png", "image/jpeg", "image/webp", "image/gif"
};
enum class Mutation { Message, Request, Operation };

void json(Response& res, int status, const Json& body) {
    res.status = status;
    res.set_content(body.dump(-1,' ',false,Json::error_handler_t::replace),"application/json; charset=utf-8");
}

void error(Response& res, int status, std::string_view code) {
    json(res,status,{{"error",code}});
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

const Json* botById(const Json& bots, std::string_view id) {
    const auto bot = std::ranges::find_if(bots,[&](const Json& descriptor) { return web::field(descriptor,"id") == id; });
    return bot == bots.end() ? nullptr : &*bot;
}

template <std::size_t N>
bool inList(const std::array<std::string_view,N>& list, std::string_view value) {
    return std::find(list.begin(),list.end(),value) != list.end();
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first,last - first + 1));
}

std::size_t utf8Codepoints(std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char ch : text)
        if ((ch & 0xC0) != 0x80) ++count;
    return count;
}

bool validHexColor(std::string_view color) {
    if ((color.size() != 4 && color.size() != 7) || color[0] != '#') return false;
    return std::all_of(color.begin() + 1,color.end(),[](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
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

std::string detectImageType(std::string_view bytes) {
    if (bytes.size() < 8 || bytes.size() > maxAvatarImageBytes) throw InputError("invalid_image");
    const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(bytes[index]); };
    if (byte(0) == 0x89 && byte(1) == 'P' && byte(2) == 'N' && byte(3) == 'G' &&
        byte(4) == 0x0D && byte(5) == 0x0A && byte(6) == 0x1A && byte(7) == 0x0A)
        return "image/png";
    if (byte(0) == 0xFF && byte(1) == 0xD8 && byte(2) == 0xFF) return "image/jpeg";
    if (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a")) return "image/gif";
    if (bytes.size() >= 12 && bytes.substr(0,4) == "RIFF" && bytes.substr(8,4) == "WEBP")
        return "image/webp";
    throw InputError("invalid_image");
}

struct ParsedProfile {
    Json profile;
    std::optional<AvatarImage> image;
};

ParsedProfile parseBotProfile(const Json& body, bool allowRole, bool requireImageForUpload) {
    const auto rawName = body.find("name");
    if (rawName == body.end() || !rawName->is_string()) throw InputError("invalid_name");
    const auto name = trim(rawName->get_ref<const std::string&>());
    if (name.empty() || name.size() > 128 || utf8Codepoints(name) > 32 ||
        std::any_of(name.begin(),name.end(),[](unsigned char ch) { return ch < 32 || ch == 127; }))
        throw InputError("invalid_name");

    Json profile = {{"name",name}};
    if (allowRole && body.contains("role")) {
        if (!body["role"].is_string() || !inList(allowedRoles,body["role"].get_ref<const std::string&>()))
            throw InputError("invalid_role");
        profile["role"] = body["role"];
    }
    const auto avatarIt = body.find("avatar");
    // Updates may omit the avatar to rename without touching the stored appearance.
    if (avatarIt == body.end() && !allowRole) return {std::move(profile),std::nullopt};
    if (avatarIt == body.end() || !avatarIt->is_object()) throw InputError("invalid_avatar");
    const auto& source = *avatarIt;
    const auto kind = web::field(source,"kind");
    std::string color = "#69c9ac";
    if (source.contains("color")) {
        if (!source["color"].is_string() || !validHexColor(source["color"].get_ref<const std::string&>()))
            throw InputError("invalid_avatar");
        color = source["color"].get<std::string>();
    }
    std::optional<AvatarImage> image;
    if (kind == "shape") {
        const auto example = web::field(source,"example");
        if (!inList(allowedShapes,example)) throw InputError("invalid_avatar");
        profile["avatar"] = {{"kind","shape"},{"example",example},{"color",color}};
    } else if (kind == "icon") {
        const auto icon = web::field(source,"icon");
        if (!inList(allowedIcons,icon)) throw InputError("invalid_avatar");
        profile["avatar"] = {{"kind","icon"},{"icon",icon},{"color",color}};
    } else if (kind == "upload") {
        Json avatar = {{"kind","upload"},{"color",color}};
        if (source.contains("filename")) {
            if (!source["filename"].is_string() || source["filename"].get_ref<const std::string&>().size() > 256)
                throw InputError("invalid_avatar");
            avatar["filename"] = source["filename"];
        }
        if (source.contains("imageData") && !source["imageData"].is_null()) {
            if (!source["imageData"].is_string()) throw InputError("invalid_image");
            auto bytes = decodeBase64(source["imageData"].get_ref<const std::string&>(),maxAvatarImageBytes);
            auto type = detectImageType(bytes);
            image = AvatarImage{std::move(type),std::move(bytes)};
        } else if (requireImageForUpload) {
            throw InputError("invalid_image");
        }
        profile["avatar"] = std::move(avatar);
    } else {
        throw InputError("invalid_avatar");
    }
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

class LoopbackServer final : public WebServer {
public:
    LoopbackServer(Store& store, unsigned port, WebCallbacks callbacks)
        : store_(store), callbacks_(std::move(callbacks)),
          changes_(store,[this](const ChangeFeed::Scope& scope) {
              return std::visit([this](const auto& target) { return snapshot(target); },scope);
          }) {
        if (port > 65535 || !callbacks_.bots || !callbacks_.intake)
            throw std::invalid_argument("Invalid Flock Web listener settings");
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
        server_.Get("/api/bots",[this](const Request&, Response& res) {
            json(res,200,botList());
        });
        server_.Get("/api/events",[this](const Request& req, Response& res) { changes(req,res); });
        server_.Get("/api/bot-avatar",[this](const Request& req, Response& res) {
            const auto id = req.get_param_value("bot");
            if (req.get_param_value_count("bot") != 1 || id.empty()) return error(res,400,"invalid_bot");
            if (!callbacks_.botAvatar) return error(res,404,"avatar_not_found");
            const auto image = callbacks_.botAvatar(id);
            if (!image || !inList(allowedImageTypes,image->contentType) || image->bytes.empty())
                return error(res,404,"avatar_not_found");
            res.set_content(image->bytes.data(),image->bytes.size(),image->contentType);
        });
        for (const auto path : {"/api/view","/api/conversation","/api/peer-conversation","/api/sessions","/api/task"}) {
            server_.Get(path,[this](const Request& req, Response& res) {
                const auto id = req.get_param_value("bot");
                if (req.get_param_value_count("bot") != 1 || id.empty()) return error(res,400,"invalid_bot");
                const auto known = bots();
                const auto* bot = botById(known,id);
                if (!bot) return error(res,404,"bot_not_found");
                try {
                    if (req.path == "/api/task") {
                        const auto taskId = queryId(req,"taskId",false);
                        const auto task = store_.task(taskId);
                        if (web::field(task,"botId") != id) return error(res,404,"task_not_found");
                        return json(res,200,web::taskDetail(task,store_.latestTaskResult(taskId)));
                    }
                    const auto before = queryId(req,"before",true);
                    if (req.path == "/api/peer-conversation") return readPeerConversation(req,res,known,id,before);
                    if (req.path == "/api/view") {
                        const auto after = req.has_param("changesAfter")
                            ? std::optional{queryId(req,"changesAfter",true)} : std::nullopt;
                        auto changes = store_.conversationChangesSince(after,id);
                        auto view = web::botView(store_,*bot);
                        view["historyCursor"] = changes.through;
                        view["changedMessageIds"] = changes.messages[id];
                        json(res,200,view);
                    } else if (req.path == "/api/conversation") json(res,200,web::conversation(store_,id,before));
                    else json(res,200,web::sessions(store_,id,before));
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
        ChangeFeed::Scope target = ChangeFeed::BotList{};
        if (!peer.empty()) target = ChangeFeed::DirectMessage{bot,peer};
        else if (!bot.empty()) target = ChangeFeed::BotView{bot};
        auto subscription = changes_.subscribe(target);
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
                if (findBot(botId).is_null()) return error(res,404,"bot_not_found");
                auto parsed = parseBotProfile(body,false,false);
                const auto saved = callbacks_.updateBot(botId,parsed.profile,parsed.image);
                changes_.rescan();
                return json(res,200,web::botSettings(saved));
            }
            auto parsed = parseBotProfile(body,true,true);
            const auto created = callbacks_.createBot(parsed.profile,parsed.image);
            changes_.rescan();
            return json(res,201,web::botSettings(created));
        } catch (const InputError& e) { error(res,400,e.what()); }
        catch (const Conflict&) { error(res,400,"invalid_bot_profile"); }
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

    std::vector<PeerBot> peers(std::string_view bot) const {
        return callbacks_.peers ? callbacks_.peers(bot) : std::vector<PeerBot>{};
    }

    bool grantsPeer(std::string_view bot, std::string_view peer) const {
        return std::ranges::any_of(peers(bot),[&](const PeerBot& entry) { return entry.intake.botId == peer; });
    }

    bool canReadPeer(std::string_view bot, std::string_view peer, bool configured) const {
        if (configured && (grantsPeer(bot,peer) || grantsPeer(peer,bot))) return true;
        const auto partners = store_.peerPartners(bot);
        return std::ranges::find(partners,peer) != partners.end();
    }

    Json snapshot(const ChangeFeed::BotList&) const { return botList(); }

    Json snapshot(const ChangeFeed::BotView& view) const {
        const auto bot = findBot(view.bot);
        return bot.is_null() ? Json(nullptr) : web::botView(store_,bot);
    }

    Json snapshot(const ChangeFeed::DirectMessage& dm) const {
        const auto known = bots();
        if (!botById(known,dm.bot) || !canReadPeer(dm.bot,dm.peer,botById(known,dm.peer) != nullptr))
            return nullptr;
        return web::peerConversation(store_,dm.bot,dm.peer,0);
    }

    Json botList() const {
        auto list = web::botList(store_,bots());
        std::set<std::string> known;
        for (const auto& bot : list) known.insert(web::field(bot,"id"));
        std::map<std::string,std::set<std::string>> connections;
        for (const auto& id : known) {
            for (const auto& entry : peers(id)) {
                const auto& peer = entry.intake.botId;
                if (id == peer || !known.contains(peer)) continue;
                connections[id].insert(peer);
                connections[peer].insert(id);
            }
            for (const auto& peer : store_.peerPartners(id))
                if (id != peer) connections[id].insert(peer);
        }
        for (auto& bot : list) {
            auto& entries = bot["peers"] = Json::array();
            for (const auto& id : connections[web::field(bot,"id")]) entries.push_back({{"id",id}});
        }
        return list;
    }

    void readPeerConversation(const Request& req, Response& res, const Json& known,
                              std::string_view bot, Id before) {
        const auto peer = req.get_param_value("peer");
        if (req.get_param_value_count("peer") != 1 || peer.empty() || bot == peer)
            return error(res,400,"invalid_peer");
        const bool configured = botById(known,peer) != nullptr;
        if (!canReadPeer(bot,peer,configured))
            return configured ? error(res,403,"peer_not_allowed") : error(res,404,"peer_not_found");
        json(res,200,web::peerConversation(store_,bot,peer,before));
    }

    void mutate(const Request& req, Response& res, Mutation mutation) {
        if (req.body.size() > maxStandardPayload) return error(res,413,"payload_too_large");
        const auto body = Json::parse(req.body,nullptr,false);
        if (!body.is_object()) return error(res,400,"invalid_json");
        try {
            const auto bot = requiredText(body,"botId",1024);
            const auto known = bots();
            if (!botById(known,bot)) return error(res,404,"bot_not_found");
            if (mutation != Mutation::Operation) {
                const auto message = mutation == Mutation::Message;
                const auto request = requiredText(body,message ? "text" : "request",16000);
                const auto clientMessageId = message ? requiredText(body,"clientMessageId",256) : std::string{};
                const auto replyTo = message && body.contains("replyToMessageId") ? bodyId(body,"replyToMessageId") : Id{};
                const auto candidate = message && body.contains("candidateTaskId") ? bodyId(body,"candidateTaskId") : Id{};
                if (candidate && !replyTo) throw InputError("invalid_candidate");
                if (message) {
                    const auto previous = store_.receipt(bot,clientMessageId,request,replyTo,candidate);
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
                intake.scheduleId.clear();
                intake.scheduledAt = 0;
                if (message) return json(res,201,store_.receive(intake,clientMessageId,request,replyTo,candidate));
                return json(res,201,{{"taskId",store_.submit(intake)}});
            }
            const auto operation = requiredText(body,"operation",32);
            if (body.contains("waitMessageId")) {
                if (operation != "approve" || body.contains("taskId") || body.contains("waitId"))
                    throw InputError("invalid_operation");
                store_.approveMessageModelCall(bot,bodyId(body,"waitMessageId"));
                return json(res,200,{{"accepted",true}});
            }
            const auto id = bodyId(body,"taskId");
            const auto wait = bodyId(body,"waitId");
            const auto data = operationData(body,operation);
            if (web::field(store_.task(id),"botId") != bot) return error(res,404,"task_not_found");
            store_.operate(id,wait,operation,data);
            json(res,200,{{"accepted",true}});
        } catch (const InputError& e) { error(res,400,e.what()); }
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
    return std::make_unique<LoopbackServer>(store,port,std::move(callbacks));
}
} // namespace flockv0
