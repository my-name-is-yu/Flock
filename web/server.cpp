#include "flock/bot_lifecycle.hpp"
#include "flock/conversation_intake.hpp"
#include "flock/task_control.hpp"
#include "flock/task_history.hpp"
#include "flock/web_conversation_intake.hpp"
#include "flock/identity.hpp"
#include "flock/bot_id.hpp"
#include "flock/decision_errors.hpp"
#include "flock/notifications.hpp"
#include "flock/peer_conversations.hpp"
#include "flock/workflows.hpp"
#include "flock/tasks.hpp"
#include "flock/task_target.hpp"
#include "flock/git_events.hpp"
#include "flock/conversations.hpp"
#include "flock/web.hpp"
#include "change_feed.hpp"
#include "embedded_assets.hpp"
#include "http_payload.hpp"
#include "views.hpp"
#include "settings_routes.hpp"
#include "board_inbox_routes.hpp"
#include "git_routes.hpp"
#include "flock/board_inbox.hpp"
#include "flock/base64.hpp"
#include "flock/limits.hpp"
#include "channel_routes.hpp"
#include "mcp_routes.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/policy_rules.hpp"
#include "flock/store.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <httplib.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace flockv0 {
namespace {
using web::ChangeFeed;
using httplib::Request;
using httplib::Response;
using HandlerResult = httplib::Server::HandlerResponse;
constexpr auto loopback = "127.0.0.1";
constexpr std::size_t messagePayloadOverheadBytes = 132 * 1024;
constexpr std::size_t maxMessagePayloadBytes = limits::messages::maxEncodedImagesBytes + messagePayloadOverheadBytes;
enum class Mutation { Message, Operation };

std::size_t requestPayloadLimit(std::string_view path) {
    if (path == "/api/messages") return maxMessagePayloadBytes;
    if (path == "/api/bots" || path == "/api/bots/profile") return web::maxUploadPayload;
    return web::maxStandardPayload;
}

void json(Response& res, int status, const Json& body) {
    res.status = status;
    res.set_content(body.dump(-1, ' ', false, Json::error_handler_t::replace), "application/json; charset=utf-8");
}

void error(Response& res, int status, std::string_view code) {
    json(res, status, {{"error", code}});
}

bool acceptsMissingPage(const Request& req) {
    if (req.method != "GET" || req.path == "/api" || req.path.starts_with("/api/") || req.path == "/mcp" || req.path.starts_with("/mcp/"))
        return false;
    const auto accept = httplib::detail::remove_quoted_strings(httplib::detail::get_combined_header_value(req.headers, "Accept"));
    bool html = false;
    httplib::detail::split(accept.data(), accept.data() + accept.size(), ',', [&](const char* begin, const char* end) {
        std::string mediaType;
        double quality;
        if (httplib::detail::parse_quality(begin, end, mediaType, quality) && quality > 0 &&
            httplib::detail::case_ignore::to_lower(mediaType) == "text/html")
            html = true;
    });
    return html;
}

void setViewer(Response& res, const WebCallbacks& callbacks, const Person& person) {
    const Json viewer = web::hasWebIdentities(callbacks) ? Json(person.id) : Json(nullptr);
    res.set_header("X-Flock-Viewer", viewer.dump(-1, ' ', true, Json::error_handler_t::replace));
}

std::string_view verifiedViewer(const WebCallbacks& callbacks, const Person& person) {
    // Local access shares one principal, so it cannot identify a human author.
    return web::hasWebIdentities(callbacks) && person.id != localOwner.id ? std::string_view{person.id} : std::string_view{};
}

struct InputError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Id queryId(const Request& req, const char* key, bool optional) {
    const auto error = optional ? "invalid_cursor" : "invalid_id";
    const auto count = req.get_param_value_count(key);
    if (count == 0 && optional) return 0;
    if (count != 1) throw InputError(error);
    const auto value = req.get_param_value(key);
    Id id = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || id < (optional ? 0 : 1)) throw InputError(error);
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
    auto text = web::field(body, key);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos || text.size() > bytes || text.find('\0') != std::string::npos)
        throw InputError("invalid_text");
    return text;
}

const Json* botById(const Json& bots, std::string_view id) {
    const auto bot = std::ranges::find_if(bots, [&](const Json& descriptor) { return web::field(descriptor, "id") == id; });
    return bot == bots.end() ? nullptr : &*bot;
}

struct ParsedProfile {
    Json profile;
    std::optional<AvatarImage> image;
};

ParsedProfile parseBotProfile(const Json& body, bool creating) {
    if (!creating && body.contains("fixPullRequests")) {
        if (body.size() != 2) throw InputError("invalid_profile_update");
        return {{{"fixPullRequests", validatedBotFixPullRequests(body.at("fixPullRequests"))}}, std::nullopt};
    }
    if (!creating && body.contains("soul")) {
        const auto original = body.find("originalSoul");
        if (body.size() != 3 || original == body.end() || !original->is_string()) throw InputError("invalid_profile_update");
        return {{{"soul", validatedBotSoul(body.at("soul"))}, {"originalSoul", *original}}, std::nullopt};
    }
    if (const auto fields = creating ? 0 : botRunSettingCount(body); fields) {
        if (body.size() != fields + 1) throw InputError("invalid_profile_update");
        Json settings = Json::object();
        for (const auto* key : botRunSettingFields) {
            if (const auto value = body.find(key); value != body.end()) settings[key] = validatedBotRunSetting(key, *value);
        }
        return {std::move(settings), std::nullopt};
    }
    Json profile = {{"name", validatedBotName(body)}};
    if (creating && body.contains("role")) {
        if (!body["role"].is_string() || !validBotRole(body["role"].get_ref<const std::string&>())) throw InputError("invalid_role");
        profile["role"] = body["role"];
    }
    if (!creating && !body.contains("avatar")) return {std::move(profile), std::nullopt};
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
            try {
                image.emplace(decodeBase64(source["imageData"].get_ref<const std::string&>(), AvatarImage::maxBytes));
            } catch (const std::invalid_argument&) {
                throw InputError("invalid_image");
            } catch (const std::length_error&) {
                throw InputError("invalid_image");
            }
        } else if (creating) {
            throw InputError("invalid_image");
        }
    }
    profile["avatar"] = std::move(avatar);
    return {std::move(profile), std::move(image)};
}

Json operationData(const Json& body, std::string_view operation) {
    if (operation == "answer") return {{"text", requiredText(body, "text", limits::messages::maxWaitAnswerBytes)}};
    if (operation == "reconcile") {
        Json data = Json::object();
        for (const auto* key : {"threadId", "runId", "questionId"}) {
            if (!body.contains(key)) continue;
            data[key] = requiredText(body, key, 1024);
        }
        if (data.contains("threadId") != data.contains("runId") || (data.contains("questionId") && !data.contains("runId")))
            throw InputError("invalid_reconciliation_target");
        return data;
    }
    if (operation != "retry" && operation != "cancel") throw InputError("invalid_operation");
    return Json::object();
}

bool hasSingleHeaderValue(const Request& req, const std::string& name, std::string_view value) {
    return req.get_header_value_count(name) == 1 && req.get_header_value(name) == value;
}

bool isLocalApprovalNavigation(const Request& req) {
    if (req.method != "GET" || req.has_header("Origin")) return false;
    if (req.get_header_value("Sec-Fetch-Mode") != "navigate" || req.get_header_value("Sec-Fetch-Dest") != "document") return false;
    std::string_view path = req.path;
    constexpr std::string_view prefix = "/settings/";
    if (!path.starts_with(prefix)) return false;
    path.remove_prefix(prefix.size());
    return validBotId(path);
}

bool isAllowedFetchSite(const Request& req) {
    const auto count = req.get_header_value_count("Sec-Fetch-Site");
    if (count == 0) return true;
    if (count != 1) return false;
    const auto site = req.get_header_value("Sec-Fetch-Site");
    return site == "same-origin" || site == "none" || (site == "cross-site" && isLocalApprovalNavigation(req));
}

class LoopbackServer final : public WebServer {
public:
    LoopbackServer(Store& store, unsigned port, WebCallbacks callbacks, const std::function<void(unsigned)>& afterBind)
        : store_(store), callbacks_(std::move(callbacks)), changes_(store, [this](const ChangeFeed::Scope& scope) {
              return std::visit([&](const auto& target) -> ChangeFeed::Projection { return snapshot(scope.person, target); }, scope.target);
          }) {
        if (port > 65535 || !callbacks_.bots || !callbacks_.intake) throw std::invalid_argument("Invalid Flock Web listener settings");
        if (callbacks_.authenticate && !callbacks_.identityRequired) {
            PolicyRules{store_}.requireWebIdentity();
            WebIdentity(store_).setWebIdentityResolver(callbacks_.resolveIdentity);
        }
        GitEvents(store_).protectGitCredential(callbacks_.githubWebhookSecret);
        server_.new_task_queue = [] { return new httplib::ThreadPool(4, 8, 32); };
        server_.set_address_family(AF_INET)
            .set_payload_max_length(std::max(web::maxUploadPayload, maxMessagePayloadBytes))
            .set_read_timeout(10)
            .set_write_timeout(10)
            .set_keep_alive_timeout(1)
            .set_default_headers(
                {{"Cache-Control", "no-store"},
                 {"Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
                                             "img-src 'self' blob:; base-uri 'none'; frame-ancestors 'none'; form-action 'none'"},
                 {"Cross-Origin-Resource-Policy", "same-origin"},
                 {"Referrer-Policy", "no-referrer"},
                 {"X-Content-Type-Options", "nosniff"},
                 {"X-Frame-Options", "DENY"}});
        server_.set_pre_routing_handler([this](const Request& req, Response& res) { return guard(req, res); });
        server_.set_exception_handler([](const Request&, Response& res, std::exception_ptr) { error(res, 500, "server_error"); });
        server_.set_error_handler([](const Request& req, Response& res) {
            if (!res.body.empty()) return;
            if (res.status == 404 && acceptsMissingPage(req)) {
                for (const auto& asset : web::assets) {
                    if (asset.path != "/not-found.html") continue;
                    res.set_content(asset.body.data(), asset.body.size(), std::string(asset.type));
                    return;
                }
            }
            error(res, res.status, "http_error");
        });
        for (const auto& asset : web::assets) {
            const auto path = asset.path == "/index.html"          ? "/"
                              : asset.path == "/settings.html"     ? "/settings"
                              : asset.path == "/bot-settings.html" ? R"(/settings/([A-Za-z0-9][A-Za-z0-9_.-]{0,127}))"
                                                                   : asset.path;
            server_.Get(std::string(path), [asset](const Request&, Response& res) {
                res.set_content(asset.body.data(), asset.body.size(), std::string(asset.type));
            });
        }
        web::settingsRoutes(server_, store_, callbacks_);
        web::memoryAutomationRoutes(server_, store_, callbacks_);
        web::boardInboxRoutes(server_, store_, callbacks_);
        web::gitRoutes(server_, store_, callbacks_);
        web::channelRoutes(server_, store_, callbacks_);
        web::mcpRoutes(server_, store_, callbacks_);
        server_.Get("/api/bots", [this](const Request& req, Response& res) {
            const auto actor = web::person(callbacks_, req);
            setViewer(res, callbacks_, actor);
            json(res, 200, botList(actor));
        });
        server_.Get("/api/events", [this](const Request& req, Response& res) { changes(req, res); });
        server_.Post("/api/notifications", [this](const Request& req, Response& res) {
            const auto actor = web::person(callbacks_, req);
            try {
                const auto body = Json::parse(req.body, nullptr, false);
                std::optional<Id> after;
                std::map<std::string, Id> scope;
                if (!body.is_object()) throw InputError("invalid_notification_cursor");
                if (!body.empty()) {
                    const auto bots = body.find("scope");
                    if (bots == body.end() || !bots->is_object() || body.size() != 2) throw InputError("invalid_notification_cursor");
                    after = bodyId(body, "after", 0);
                    for (const auto& [bot, unused] : bots->items()) {
                        if (!validBotId(bot)) throw InputError("invalid_notification_cursor");
                        scope.emplace(bot, bodyId(*bots, bot, 0));
                    }
                }
                setViewer(res, callbacks_, actor);
                json(res, 200, notificationPage(actor, after, scope));
            } catch (const InputError& e) {
                error(res, 400, e.what());
            }
        });
        server_.Get("/api/bot-avatar", [this](const Request& req, Response& res) {
            const auto id = req.get_param_value("bot");
            if (req.get_param_value_count("bot") != 1 || id.empty()) return error(res, 400, "invalid_bot");
            if (!Policy{store_}.allows(web::person(callbacks_, req), Action::bot_view, id)) return error(res, 403, "access_denied");
            if (!callbacks_.botAvatar) return error(res, 404, "avatar_not_found");
            const auto image = callbacks_.botAvatar(id);
            if (!image) return error(res, 404, "avatar_not_found");
            res.set_content(image->bytes().data(), image->bytes().size(), std::string(image->format().contentType));
        });
        for (const auto path :
             {"/api/view", "/api/workflows", "/api/compactions", "/api/conversation", "/api/peer-conversation", "/api/sessions"}) {
            server_.Get(path, [this](const Request& req, Response& res) {
                const auto id = req.get_param_value("bot");
                if (req.get_param_value_count("bot") != 1 || id.empty()) return error(res, 400, "invalid_bot");
                const auto actor = web::person(callbacks_, req);
                if (!Policy{store_}.allows(actor, Action::bot_view, id)) return error(res, 403, "access_denied");
                const auto known = bots();
                const auto* bot = botById(known, id);
                if (!bot) return error(res, 404, "bot_not_found");
                try {
                    const auto before = queryId(req, "before", true);
                    if (req.path == "/api/peer-conversation") return readPeerConversation(req, res, known, actor, id, before);
                    if (req.path == "/api/compactions" || req.path == "/api/workflows") {
                        const auto items = req.path == "/api/compactions" ? Conversations(store_).compactions(id, before)
                                                                          : Workflows(store_).workflows(id, before);
                        return json(res, 200, Policy{store_}.redactSecrets(id, items));
                    }
                    if (req.path == "/api/view") {
                        const auto changesAfter =
                            req.has_param("changesAfter") ? std::optional{queryId(req, "changesAfter", true)} : std::nullopt;
                        const auto sessionsAfter =
                            req.has_param("sessionsAfter") ? std::optional{queryId(req, "sessionsAfter", true)} : std::nullopt;
                        const auto changes = Conversations(store_).conversationChangesSince(changesAfter, id);
                        const auto [sessionThrough, sessionTasks] = TaskHistory(store_).sessionChangesSince(id, sessionsAfter);
                        auto view = readBotView(actor, id, *bot, sessionsAfter ? &sessionTasks : nullptr);
                        view["bot"]["id"] = id;
                        view["limits"] = {{"messageBytes", limits::messages::maxBodyBytes},
                                          {"waitAnswerBytes", limits::messages::maxWaitAnswerBytes},
                                          {"attachments", limits::messages::maxAttachments}};
                        view["composerPerson"] = web::hasWebIdentities(callbacks_) ? Json(actor.id) : Json(nullptr);
                        view["historyCursor"] = changes.through;
                        view["changedMessageIds"] = Json::array();
                        if (const auto changed = changes.messages.find(id); changed != changes.messages.end())
                            view["changedMessageIds"] = changed->second;
                        view["sessionCursor"] = sessionThrough;
                        setViewer(res, callbacks_, actor);
                        json(res, 200, withConversationAccess(actor, id, std::move(view)));
                    } else if (req.path == "/api/conversation")
                        json(res, 200, Policy{store_}.redactSecrets(id, web::conversation(store_, id, before, actor.id)));
                    else
                        json(res, 200, Policy{store_}.redactSecrets(id, web::sessions(store_, id, before)));
                } catch (const InputError& e) {
                    error(res, 400, e.what());
                } catch (const Conflict&) {
                    error(res, 404, "task_not_found");
                }
            });
        }
        server_.Post("/api/bots", [this](const Request& req, Response& res) { saveBot(req, res, false); });
        server_.Post("/api/bots/profile", [this](const Request& req, Response& res) { saveBot(req, res, true); });
        server_.Post("/api/bots/delete", [this](const Request& req, Response& res) { deleteBot(req, res); });
        server_.Post("/api/bots/pause", [this](const Request& req, Response& res) { setBotPaused(req, res); });
        server_.Post("/api/messages", [this](const Request& req, Response& res) { mutate(req, res, Mutation::Message); });
        server_.Post("/api/operations", [this](const Request& req, Response& res) { mutate(req, res, Mutation::Operation); });

        const auto bound = port ? (server_.bind_to_port(loopback, static_cast<int>(port)) ? static_cast<int>(port) : -1)
                                : server_.bind_to_any_port(loopback);
        if (bound <= 0) {
            server_.stop();
            throw std::runtime_error(port ? "Could not bind the Flock Web loopback listener on port " + std::to_string(port) +
                                                ". Check whether the port is in use."
                                          : "Could not bind the Flock Web loopback listener");
        }
        port_ = static_cast<unsigned>(bound);
        std::promise<bool> serve;
        try {
            worker_ = std::thread([this, ready = serve.get_future()]() mutable {
                if (ready.get()) server_.listen_after_bind();
            });
            if (afterBind) afterBind(port_);
            serve.set_value(true);
        } catch (...) {
            server_.stop();
            if (worker_.joinable()) {
                serve.set_value(false);
                worker_.join();
            }
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
        WebIdentity(store_).setWebOrigin({});
    }
    unsigned port() const override { return port_; }
private:
    void changes(const Request& req, Response& res) {
        const auto bot = req.get_param_value("bot");
        const auto peer = req.get_param_value("peer");
        if (req.get_param_value_count("bot") > 1 || req.get_param_value_count("peer") > 1 || bot.size() > 1024 || peer.size() > 1024 ||
            (req.has_param("bot") && bot.empty()) || (req.has_param("peer") && (bot.empty() || peer.empty() || bot == peer)))
            return error(res, 400, "invalid_subscription");
        const auto person = web::person(callbacks_, req);
        Policy policy{store_};
        if ((!bot.empty() && !policy.allows(person, Action::bot_view, bot)) ||
            (!peer.empty() && !policy.allows(person, Action::bot_view, peer)))
            return error(res, 403, "access_denied");
        ChangeFeed::Target target = ChangeFeed::BotList{};
        if (!peer.empty())
            target = ChangeFeed::DirectMessage{bot, peer};
        else if (!bot.empty())
            target = ChangeFeed::BotView{bot};
        auto subscription = changes_.subscribe({person, std::move(target)});
        if (!subscription) return error(res, 503, "event_stream_unavailable");
        auto cursor = std::make_shared<web::ChangeFeed::Cursor>(std::move(*subscription));
        auto authorization = callbacks_.identityRequired ? req.get_header_value("Authorization") : std::string{};
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, cursor, person, authorization = std::move(authorization)](std::size_t offset, httplib::DataSink& sink) {
                if (!sink.is_writable()) return false;
                const auto authorized = [&] { return !callbacks_.identityRequired || callbacks_.authenticate(authorization) == person; };
                if (!authorized()) {
                    sink.done();
                    return true;
                }
                if (offset == 0) {
                    constexpr std::string_view ready = "retry: 2500\nevent: ready\ndata: {}\n\n";
                    return sink.write(ready.data(), ready.size());
                }
                const auto update = changes_.next(*cursor);
                if (!update || !authorized()) {
                    sink.done();
                    return true;
                }
                if (!sink.is_writable()) return false;
                const auto event = update->empty()
                                       ? std::string("event: ping\ndata: {}\n\n")
                                       : "event: change\ndata: " + update->dump(-1, ' ', false, Json::error_handler_t::replace) + "\n\n";
                return sink.write(event.data(), event.size());
            },
            [this, cursor](bool) { changes_.release(*cursor); });
    }

    HandlerResult guard(const Request& req, Response& res) const {
        const auto reject = [&](int status, std::string_view code) {
            error(res, status, code);
            return HandlerResult::Handled;
        };
        const auto portSuffix = port_ == 80 ? std::string{} : ":" + std::to_string(port_);
        const auto host = req.get_header_value("Host");
        if (req.get_header_value_count("Host") != 1 || (host != loopback + portSuffix && host != "localhost" + portSuffix))
            return reject(403, "invalid_host");

        const bool readOnlyMethod = req.method == "GET" || req.method == "HEAD";
        if (readOnlyMethod && web::artifactPreview(req.path)) return HandlerResult::Unhandled;

        const auto originCount = req.get_header_value_count("Origin");
        const bool isSameOriginRequest = originCount == 1 && req.get_header_value("Origin") == "http://" + host;
        if (originCount != 0 && !isSameOriginRequest) return reject(403, "invalid_origin");
        if (!isAllowedFetchSite(req)) return reject(403, "invalid_origin");

        const bool hasBrowserOriginHeaders = originCount != 0 || req.has_header("Sec-Fetch-Site");
        const bool post = req.method == "POST";
        if (req.path == web::sessionMcpPath || req.path == web::externalMcpPath) {
            if (hasBrowserOriginHeaders) return reject(403, "invalid_origin");
            if (!post && req.method != "GET" && req.method != "DELETE") {
                res.set_header("Allow", "POST");
                return reject(405, "method_not_allowed");
            }
            return HandlerResult::Unhandled;
        }
        if (post && !hasBrowserOriginHeaders && req.path.starts_with("/api/git/webhooks/")) return HandlerResult::Unhandled;
        if (post && web::bridgeEndpoint(req.path)) {
            if (hasBrowserOriginHeaders) return reject(403, "invalid_origin");
            if (!hasSingleHeaderValue(req, "Content-Type", "application/json")) return reject(415, "expected_json");
            return HandlerResult::Unhandled;
        }

        if (web::person(callbacks_, req).id.empty() || (!callbacks_.authenticate && Policy{store_}.requiresIdentity())) {
            res.set_header("WWW-Authenticate", "Basic realm=\"Flock\"");
            return reject(401, "identity_required");
        }
        if (readOnlyMethod) return HandlerResult::Unhandled;

        const bool deletingSchedule = req.method == "DELETE" && req.path == "/api/schedules";
        if (!post && !deletingSchedule) return reject(405, "method_not_allowed");
        const bool isMachineWebhook = !hasBrowserOriginHeaders && req.path.starts_with("/api/webhooks/");
        if (!isSameOriginRequest && !isMachineWebhook) return reject(403, "invalid_origin");
        if (!hasSingleHeaderValue(req, "X-Flock-Action", "1")) return reject(403, "invalid_origin");
        if (deletingSchedule) return HandlerResult::Unhandled;

        if (!hasSingleHeaderValue(req, "Content-Type", "application/json")) return reject(415, "expected_json");
        const auto lengthHeader = req.get_header_value("Content-Length");
        if (!lengthHeader.empty()) {
            std::size_t contentLength = 0;
            const auto end = lengthHeader.data() + lengthHeader.size();
            const auto parsed = std::from_chars(lengthHeader.data(), end, contentLength);
            if (parsed.ec == std::errc{} && parsed.ptr == end && contentLength > requestPayloadLimit(req.path))
                return reject(413, "payload_too_large");
        }
        return HandlerResult::Unhandled;
    }

    void saveBot(const Request& req, Response& res, bool update) {
        if (req.body.size() > requestPayloadLimit(req.path)) return error(res, 413, "payload_too_large");
        if (update ? !callbacks_.updateBot : !callbacks_.createBot) return error(res, 501, "bot_management_unavailable");
        const auto body = Json::parse(req.body, nullptr, false);
        if (!body.is_object()) return error(res, 400, "invalid_json");
        try {
            if (update) {
                const auto botId = requiredText(body, "botId", 1024);
                if (!Policy{store_}.allows(web::person(callbacks_, req), Action::bot_settings, botId))
                    return error(res, 403, "access_denied");
                if (findBot(botId).is_null()) return error(res, 404, "bot_not_found");
                auto parsed = parseBotProfile(body, false);
                const auto redacted = Policy{store_}.redactSecrets(botId, parsed.profile);
                if (const auto original = parsed.profile.find("originalSoul");
                    original != parsed.profile.end() && redacted.at("originalSoul") != *original)
                    return error(res, 409, "role_changed");
                if (redacted != parsed.profile)
                    return error(res, 400, parsed.profile.contains("soul") ? "role_contains_secrets" : "invalid_bot_profile");
                const auto saved = callbacks_.updateBot(botId, parsed.profile, parsed.image);
                changes_.rescan();
                return json(res, 200, redactBot(botId, web::botSettings(saved)));
            }
            const auto creator = web::person(callbacks_, req);
            if (!Policy{store_}.allows(creator, Action::bot_create, "")) return error(res, 403, "access_denied");
            auto parsed = parseBotProfile(body, true);
            const auto created = callbacks_.createBot(creator, parsed.profile, parsed.image);
            changes_.rescan();
            return json(res, 201, redactBot(web::field(created, "id"), web::botSettings(created)));
        } catch (const InputError& e) {
            error(res, 400, e.what());
        } catch (const BotProfileError& e) {
            error(res, e.code == "role_changed" ? 409 : 400, e.code);
        } catch (const Conflict&) {
            error(res, 400, "invalid_bot_profile");
        }
    }

    void deleteBot(const Request& req, Response& res) {
        if (req.body.size() > web::maxStandardPayload) return error(res, 413, "payload_too_large");
        const auto body = Json::parse(req.body, nullptr, false);
        if (!body.is_object()) return error(res, 400, "invalid_json");
        const auto bot = web::field(body, "botId");
        if (!validBotId(bot)) return error(res, 400, "invalid_bot");
        const auto actor = web::person(callbacks_, req);
        if (findBot(bot).is_null()) {
            if (!Policy{store_}.allows(actor, Action::bot_settings, bot)) return error(res, 403, "access_denied");
            return error(res, 404, "bot_not_found");
        }
        try {
            BotLifecycle(store_).deleteBot(actor, bot);
        } catch (const DecisionDenied&) {
            return error(res, 403, "access_denied");
        } catch (const BotBusy&) {
            return error(res, 409, "bot_busy");
        }
        changes_.rescan();
        json(res, 200, {{"ok", true}});
    }

    void setBotPaused(const Request& req, Response& res) {
        if (req.body.size() > web::maxStandardPayload) return error(res, 413, "payload_too_large");
        const auto body = Json::parse(req.body, nullptr, false);
        if (!body.is_object()) return error(res, 400, "invalid_json");
        const auto bot = web::field(body, "botId");
        if (!validBotId(bot)) return error(res, 400, "invalid_bot");
        const auto paused = body.find("paused");
        if (paused == body.end() || !paused->is_boolean()) return error(res, 400, "invalid_paused");
        const auto actor = web::person(callbacks_, req);
        if (!Policy{store_}.allows(actor, Action::bot_settings, bot)) return error(res, 403, "access_denied");
        if (findBot(bot).is_null()) return error(res, 404, "bot_not_found");
        try {
            BotLifecycle(store_).setBotPaused(actor, bot, paused->get<bool>());
        } catch (const DecisionDenied&) {
            return error(res, 403, "access_denied");
        }
        changes_.rescan();
        json(res, 200, {{"paused", *paused}});
    }

    Json bots() const {
        auto list = callbacks_.bots();
        if (!list.is_array()) throw std::runtime_error("Bot descriptors must be an array");
        return list;
    }

    Json redactBot(std::string_view id, const Json& view) const {
        auto bot = Policy{store_}.redactSecrets(id, view);
        bot["id"] = id;
        return bot;
    }

    Json findBot(std::string_view id) const {
        for (auto& bot : bots())
            if (web::field(bot, "id") == id) return std::move(bot);
        return nullptr;
    }

    std::vector<PeerBot> peers(std::string_view bot) const { return callbacks_.peers ? callbacks_.peers(bot) : std::vector<PeerBot>{}; }

    bool hasPeerPermission(std::string_view bot, std::string_view peer) const {
        return std::ranges::any_of(peers(bot), [&](const PeerBot& entry) { return entry.intake.botId == peer; });
    }

    bool canReadPeer(std::string_view bot, std::string_view peer, bool configured) const {
        if (configured && (hasPeerPermission(bot, peer) || hasPeerPermission(peer, bot))) return true;
        return PeerConversations(store_).hasPeerExchange(bot, peer);
    }

    Json snapshot(const Person& person, const ChangeFeed::BotList&) const {
        return {{"bots", botList(person)}, {"inbox", Inbox(store_).inbox(person, {})}};
    }

    Json notificationPage(const Person& person, std::optional<Id> after, const std::map<std::string, Id>& scope) const {
        std::map<std::string, std::string> names;
        std::vector<std::string> configured;
        for (const auto& bot : bots()) {
            const auto id = web::field(bot, "id");
            configured.push_back(id);
            names.emplace(id, web::field(bot, "name"));
        }
        auto page = Notifications(store_).notificationEventsSince(person, configured, after, scope);
        for (const auto& [bot, cursor] : page.scope) names.at(bot) = web::field(redactBot(bot, {{"name", names.at(bot)}}), "name");
        auto inbox = Json::array();
        for (const auto& item : Inbox(store_).inbox(person, {})) {
            const auto id = web::field(item, "botId");
            if (!page.scope.contains(id) || !item.value("canDecide", false) || web::field(item, "status") == "expired") continue;
            const auto kind = web::field(item, "itemKind");
            inbox.push_back({{"id", item.at("itemId")}, {"botId", id}, {"botName", names.at(id)}, {"kind", kind}});
        }
        for (auto& event : page.events) event["botName"] = names.at(web::field(event, "botId"));
        return {{"through", page.through},
                {"scope", std::move(page.scope)},
                {"more", page.more},
                {"events", std::move(page.events)},
                {"inbox", std::move(inbox)}};
    }

    Json withConversationAccess(const Person& actor, std::string_view bot, Json view) const {
        Policy policy{store_};
        view["canDecide"] = policy.allows(actor, Action::bot_decide, bot);
        view["bot"]["canDelete"] = policy.allows(actor, Action::bot_settings, bot);
        view["bot"]["paused"] = BotLifecycle(store_).botPaused(bot);
        return view;
    }

    ChangeFeed::Projection snapshot(const Person& person, const ChangeFeed::BotView& view) const {
        const auto stamp = Notifications(store_).botViewStamp();
        if (!Policy{store_}.allows(person, Action::bot_view, view.bot)) return Json(nullptr);
        auto bot = findBot(view.bot);
        if (bot.is_null()) return bot;
        auto data =
            withConversationAccess(person, view.bot, Policy{store_}.redactSecrets(view.bot, web::botView(store_, bot, nullptr, person.id)));
        if (stamp && stamp == Notifications(store_).botViewStamp())
            return {std::move(data), ChangeFeed::BotVersion{*stamp, std::move(bot)}};
        return data;
    }

    Json readBotView(const Person& person, const std::string& id, const Json& bot, const std::vector<TaskView>* changedSessionTasks) {
        Policy policy{store_};
        if (const auto cached = changes_.cachedBotView(person, id, bot)) {
            // Workflow deadlines advance without a write. Session deltas belong to the request's cursor.
            std::optional<Json> workflows, sessions;
            if (!cached->data.at("workflows").empty()) workflows = policy.redactSecrets(id, Workflows(store_).workflows(id));
            if (changedSessionTasks) sessions = policy.redactSecrets(id, web::sessionGroups(*changedSessionTasks));
            if (Notifications(store_).botViewStamp() == cached->botVersion->stamp) {
                auto view = cached->data;
                if (workflows) view["workflows"] = std::move(*workflows);
                if (sessions) {
                    view["sessionTasks"] = std::move(*sessions);
                    view.erase("sessionsBefore");
                }
                return view;
            }
        }
        return policy.redactSecrets(id, web::botView(store_, bot, changedSessionTasks, person.id));
    }

    Json snapshot(const Person& person, const ChangeFeed::DirectMessage& dm) const {
        Policy policy{store_};
        if (!policy.allows(person, Action::bot_view, dm.bot) || !policy.allows(person, Action::bot_view, dm.peer)) return nullptr;
        const auto known = bots();
        if (!botById(known, dm.bot) || !canReadPeer(dm.bot, dm.peer, botById(known, dm.peer) != nullptr)) return nullptr;
        return policy.redactSecrets(dm.peer, policy.redactSecrets(dm.bot, web::peerConversation(store_, dm.bot, dm.peer, 0)));
    }

    Json botList(const Person& person) const {
        auto visible = bots();
        Policy policy{store_};
        visible.erase(std::remove_if(visible.begin(), visible.end(),
                                     [&](const Json& bot) { return !policy.allows(person, Action::bot_view, web::field(bot, "id")); }),
                      visible.end());
        auto list = web::botList(store_, visible);
        if (list.empty()) return list;
        std::set<std::string> known;
        for (const auto& bot : list) known.insert(web::field(bot, "id"));
        auto pairs = PeerConversations(store_).peerExchangePairs();
        for (const auto& id : known) {
            for (const auto& entry : peers(id)) {
                const auto& peer = entry.intake.botId;
                if (known.contains(peer)) pairs.emplace_back(id, peer);
            }
        }
        std::map<std::string, std::set<std::string>> connections;
        for (const auto& [sender, recipient] : pairs) {
            if (sender == recipient) continue;
            if (known.contains(sender) && policy.allows(person, Action::bot_view, recipient)) connections[sender].insert(recipient);
            if (known.contains(recipient) && policy.allows(person, Action::bot_view, sender)) connections[recipient].insert(sender);
        }
        for (auto& bot : list) {
            const auto id = web::field(bot, "id");
            bot = redactBot(id, bot);
            bot["canDelete"] = policy.allows(person, Action::bot_settings, id);
            bot["paused"] = BotLifecycle(store_).botPaused(id);
            if (auto about = bot.find("about"); about != bot.end()) {
                const auto& soul = about->get_ref<const std::string&>();
                const auto firstLine = std::string_view(soul).substr(0, soul.find_first_of("\r\n"));
                if (firstLine.empty())
                    bot.erase(about);
                else
                    *about = bounded(firstLine, 160);
            }
            auto& entries = bot["peers"] = Json::array();
            for (const auto& peer : connections[id]) entries.push_back({{"id", peer}});
        }
        return list;
    }

    void readPeerConversation(const Request& req, Response& res, const Json& known, const Person& actor, std::string_view bot, Id before) {
        const auto peer = req.get_param_value("peer");
        if (req.get_param_value_count("peer") != 1 || peer.empty() || bot == peer) return error(res, 400, "invalid_peer");
        if (!Policy{store_}.allows(actor, Action::bot_view, peer)) return error(res, 403, "access_denied");
        const bool configured = botById(known, peer) != nullptr;
        if (!canReadPeer(bot, peer, configured))
            return configured ? error(res, 403, "peer_not_allowed") : error(res, 404, "peer_not_found");
        Id anchor = 0;
        if (req.has_param("anchor")) {
            if (req.has_param("before")) return error(res, 400, "invalid_cursor");
            anchor = PeerConversations(store_).peerMessageAnchor(bot, peer, queryId(req, "anchor", false));
            if (!anchor) return error(res, 404, "peer_message_not_found");
        }
        const auto history =
            Policy{store_}.redactSecrets(bot, web::peerConversation(store_, bot, peer, before, anchor, verifiedViewer(callbacks_, actor)));
        json(res, 200, Policy{store_}.redactSecrets(peer, history));
    }

    void mutate(const Request& req, Response& res, Mutation mutation) {
        if (req.body.size() > requestPayloadLimit(req.path)) return error(res, 413, "payload_too_large");
        const auto body = Json::parse(req.body, nullptr, false);
        if (!body.is_object()) return error(res, 400, "invalid_json");
        try {
            const auto bot = requiredText(body, "botId", 1024);
            const auto actor = web::person(callbacks_, req);
            if (!Policy{store_}.allows(actor, Action::bot_decide, bot)) return error(res, 403, "access_denied");
            if (!botById(bots(), bot)) return error(res, 404, "bot_not_found");
            if (mutation == Mutation::Message) {
                const auto request = requiredText(body, "text", limits::messages::maxBodyBytes);
                const auto clientMessageId = requiredText(body, "clientMessageId", 256);
                const auto replyTo = body.contains("replyToMessageId") ? bodyId(body, "replyToMessageId") : Id{};
                const auto attachments = body.value("attachments", Json::array());
                const auto previous = WebConversationIntake(store_).receipt(actor, bot, clientMessageId, request, replyTo, attachments);
                if (previous) return json(res, 201, *previous);
                const bool identityRequired = web::hasWebIdentities(callbacks_);
                if (identityRequired && !callbacks_.resolveIdentity) return error(res, 503, "message_identity_unavailable");
                Intake intake;
                try {
                    intake = callbacks_.intake(bot, request);
                } catch (const std::exception&) {
                }
                // The caller supplies only a Bot and text. Routing and config come from the callback.
                intake.botId = bot;
                intake.request = request;
                const auto receipt =
                    identityRequired
                        ? WebConversationIntake(store_).receiveWeb(actor, intake, clientMessageId, request, replyTo, attachments)
                        : ConversationIntake(store_).receive(actor, intake, clientMessageId, request, replyTo, attachments);
                return json(res, 201, receipt);
            }
            const auto operation = requiredText(body, "operation", 32);
            const auto id = bodyId(body, "taskId");
            const auto wait = bodyId(body, "waitId");
            auto data = operationData(body, operation);
            const auto task = Tasks(store_).task(id);
            if (task.task.botId != bot) return error(res, 404, "task_not_found");
            if (operation == "reconcile") data = TaskControl(store_).reconciliationData(id, wait, std::move(data), callbacks_.intake);
            TaskControl(store_).operate(actor, {bot, id, wait, bodyId(body, "targetRevision", 0), bodyId(body, "targetGeneration", 0)},
                                        operation, data);
            json(res, 200, {{"accepted", true}});
        } catch (const InputError& e) {
            error(res, 400, e.what());
        } catch (const DecisionDenied&) {
            error(res, 403, "access_denied");
        } catch (const BotPaused& e) {
            json(res, 409, {{"error", "bot_paused"}, {"message", e.what()}});
        } catch (const RequestChanged&) {
            error(res, 409, "request_changed");
        } catch (const Conflict&) {
            error(res, 409, mutation == Mutation::Message ? "message_conflict" : "operation_conflict");
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

std::unique_ptr<WebServer> startWeb(Store& store, unsigned port, WebCallbacks callbacks, std::function<void(unsigned)> afterBind) {
    auto server = std::make_unique<LoopbackServer>(store, port, std::move(callbacks), afterBind);
    WebIdentity(store).setWebOrigin("http://" + std::string(loopback) + ":" + std::to_string(server->port()));
    return server;
}
} // namespace flockv0
