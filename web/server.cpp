#include "flock/bot_lifecycle.hpp"
#include "flock/conversation_intake.hpp"
#include "flock/task_control.hpp"
#include "flock/task_history.hpp"
#include "flock/web_conversation_intake.hpp"
#include "flock/identity.hpp"
#include "flock/bot_id.hpp"
#include "flock/avatar_style.hpp"
#include "flock/decision_errors.hpp"
#include "flock/notifications.hpp"
#include "flock/tasks.hpp"
#include "flock/task_target.hpp"
#include "flock/git_events.hpp"
#include "flock/conversations.hpp"
#include "flock/web.hpp"
#include "flock/bot_setup.hpp"
#include "change_feed.hpp"
#include "embedded_assets.hpp"
#include "http.hpp"
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
using web::error;
using web::reply;
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
    auto text = field(body, key);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos || text.size() > bytes || text.find('\0') != std::string::npos)
        throw InputError("invalid_text");
    return text;
}

const Json* botById(const Json& bots, std::string_view id) {
    const auto bot = std::ranges::find_if(bots, [&](const Json& descriptor) { return field(descriptor, "id") == id; });
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
    if (!creating && (body.contains("sessionModel") || body.contains("workspaceStrategy"))) {
        auto profile = body;
        profile.erase("botId");
        for (auto it = profile.begin(); it != profile.end(); ++it)
            if (it.key() != "sessionModel" && it.key() != "workspaceStrategy") throw InputError("invalid_profile_update");
        return {std::move(profile), std::nullopt};
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
    if (creating && (body.contains("environmentId") || body.contains("projectId"))) {
        for (const auto* key : {"environmentId", "projectId"}) {
            if (!body.contains(key)) throw InputError("invalid_workspace");
            profile[key] = validatedBotRunSetting(key, body.at(key));
            if (profile.at(key) == "") throw InputError("invalid_workspace");
        }
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
    if (operation != "cancel") throw InputError("invalid_operation");
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
            reply(res, 200, botList(actor));
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
                reply(res, 200, notificationPage(actor, after, scope));
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
        for (const auto path : {"/api/view", "/api/compactions", "/api/conversation", "/api/sessions"}) {
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
                    if (req.path == "/api/compactions")
                        return reply(res, 200, Policy{store_}.redactSecrets(id, Conversations(store_).compactions(id, before)));
                    if (req.path == "/api/view") {
                        const auto changesAfter =
                            req.has_param("changesAfter") ? std::optional{queryId(req, "changesAfter", true)} : std::nullopt;
                        const auto sessionsAfter =
                            req.has_param("sessionsAfter") ? std::optional{queryId(req, "sessionsAfter", true)} : std::nullopt;
                        const auto changes = Conversations(store_).conversationChangesSince(changesAfter, id);
                        const auto [sessionThrough, sessionTasks] = TaskHistory(store_).finishedTaskChangesSince(id, sessionsAfter);
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
                        reply(res, 200, withConversationAccess(actor, id, std::move(view)));
                    } else if (req.path == "/api/conversation")
                        reply(res, 200, Policy{store_}.redactSecrets(id, web::conversation(store_, id, before, &actor)));
                    else
                        reply(res, 200, Policy{store_}.redactSecrets(id, web::sessions(store_, id, before)));
                } catch (const InputError& e) {
                    error(res, 400, e.what());
                } catch (const Conflict&) {
                    error(res, 404, "task_not_found");
                }
            });
        }
        server_.Post("/api/bots", [this](const Request& req, Response& res) { saveBot(req, res, false); });
        server_.Post("/api/bots/profile", [this](const Request& req, Response& res) { saveBot(req, res, true); });
        server_.Post("/api/bots/setup/undo", [this](const Request& req, Response& res) {
            try {
                const auto body = Json::parse(req.body, nullptr, false);
                if (!body.is_object()) return error(res, 400, "invalid_json");
                undoBotSetup(store_, web::person(callbacks_, req), requiredText(body, "botId", 128), bodyId(body, "eventId"));
                changes_.rescan();
                reply(res, 200, {{"ok", true}});
            } catch (const InputError& e) {
                error(res, 400, e.what());
            } catch (const AccessDenied&) {
                error(res, 403, "access_denied");
            } catch (const BotProfileError& e) {
                const bool stale = e.code == "setup_changed" || e.code == "role_changed";
                error(res, stale ? 409 : 500, stale ? "setup_changed" : "setup_undo_failed");
            } catch (const Conflict&) {
                error(res, 500, "setup_undo_failed");
            }
        });
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
        if (req.get_param_value_count("bot") > 1 || bot.size() > 1024 || (req.has_param("bot") && bot.empty()))
            return error(res, 400, "invalid_subscription");
        const auto person = web::person(callbacks_, req);
        Policy policy{store_};
        if (!bot.empty() && !policy.allows(person, Action::bot_view, bot)) return error(res, 403, "access_denied");
        ChangeFeed::Target target = ChangeFeed::BotList{};
        if (!bot.empty()) target = ChangeFeed::BotView{bot};
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
                const auto actor = web::person(callbacks_, req);
                if (!Policy{store_}.allows(actor, Action::bot_settings, botId)) return error(res, 403, "access_denied");
                if (findBot(botId).is_null()) return error(res, 404, "bot_not_found");
                auto parsed = parseBotProfile(body, false);
                const auto redacted = Policy{store_}.redactSecrets(botId, parsed.profile);
                if (const auto original = parsed.profile.find("originalSoul");
                    original != parsed.profile.end() && redacted.at("originalSoul") != *original)
                    return error(res, 409, "role_changed");
                if (redacted != parsed.profile)
                    return error(res, 400, parsed.profile.contains("soul") ? "role_contains_secrets" : "invalid_bot_profile");
                const auto saved = callbacks_.updateBot(actor, botId, parsed.profile, parsed.image);
                changes_.rescan();
                return reply(res, 200, redactBot(botId, web::botSettings(saved)));
            }
            const auto creator = web::person(callbacks_, req);
            if (!Policy{store_}.allows(creator, Action::bot_create, "")) return error(res, 403, "access_denied");
            auto parsed = parseBotProfile(body, true);
            if (parsed.profile.contains("projectId")) {
                const auto environment = field(parsed.profile, "environmentId");
                const auto project = field(parsed.profile, "projectId");
                bool available;
                try {
                    available = web::projectAvailable(store_, callbacks_, creator, environment, project);
                } catch (const std::exception&) {
                    return error(res, 503, "workspaces_unavailable");
                }
                if (!available) throw InputError("invalid_workspace");
            }
            const auto created = callbacks_.createBot(creator, parsed.profile, parsed.image);
            changes_.rescan();
            return reply(res, 201, redactBot(field(created, "id"), web::botSettings(created)));
        } catch (const InputError& e) {
            error(res, 400, e.what());
        } catch (const BotProfileError& e) {
            error(res, e.code == "role_changed" ? 409 : 400, e.code);
        } catch (const AccessDenied&) {
            error(res, 403, "access_denied");
        } catch (const Conflict&) {
            error(res, 400, "invalid_bot_profile");
        }
    }

    void deleteBot(const Request& req, Response& res) {
        if (req.body.size() > web::maxStandardPayload) return error(res, 413, "payload_too_large");
        const auto body = Json::parse(req.body, nullptr, false);
        if (!body.is_object()) return error(res, 400, "invalid_json");
        const auto bot = field(body, "botId");
        if (!validBotId(bot)) return error(res, 400, "invalid_bot");
        const auto actor = web::person(callbacks_, req);
        if (findBot(bot).is_null()) {
            if (!Policy{store_}.allows(actor, Action::bot_settings, bot)) return error(res, 403, "access_denied");
            return error(res, 404, "bot_not_found");
        }
        try {
            BotLifecycle(store_).deleteBot(actor, bot);
        } catch (const AccessDenied&) {
            return error(res, 403, "access_denied");
        } catch (const BotBusy&) {
            return error(res, 409, "bot_busy");
        }
        changes_.rescan();
        reply(res, 200, {{"ok", true}});
    }

    void setBotPaused(const Request& req, Response& res) {
        if (req.body.size() > web::maxStandardPayload) return error(res, 413, "payload_too_large");
        const auto body = Json::parse(req.body, nullptr, false);
        if (!body.is_object()) return error(res, 400, "invalid_json");
        const auto bot = field(body, "botId");
        if (!validBotId(bot)) return error(res, 400, "invalid_bot");
        const auto paused = body.find("paused");
        if (paused == body.end() || !paused->is_boolean()) return error(res, 400, "invalid_paused");
        const auto actor = web::person(callbacks_, req);
        if (!Policy{store_}.allows(actor, Action::bot_settings, bot)) return error(res, 403, "access_denied");
        if (findBot(bot).is_null()) return error(res, 404, "bot_not_found");
        try {
            BotLifecycle(store_).setBotPaused(actor, bot, paused->get<bool>());
        } catch (const AccessDenied&) {
            return error(res, 403, "access_denied");
        }
        changes_.rescan();
        reply(res, 200, {{"paused", *paused}});
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
            if (field(bot, "id") == id) return std::move(bot);
        return nullptr;
    }

    Json snapshot(const Person& person, const ChangeFeed::BotList&) const {
        return {{"bots", botList(person)}, {"inbox", Inbox(store_).inbox(person, {})}};
    }

    Json notificationPage(const Person& person, std::optional<Id> after, const std::map<std::string, Id>& scope) const {
        std::map<std::string, std::string> names;
        std::vector<std::string> configured;
        for (const auto& bot : bots()) {
            const auto id = field(bot, "id");
            configured.push_back(id);
            names.emplace(id, field(bot, "name"));
        }
        auto page = Notifications(store_).notificationEventsSince(person, configured, after, scope);
        for (const auto& [bot, cursor] : page.scope) names.at(bot) = field(redactBot(bot, {{"name", names.at(bot)}}), "name");
        auto inbox = Json::array();
        for (const auto& item : Inbox(store_).inbox(person, {})) {
            const auto id = field(item, "botId");
            if (!page.scope.contains(id) || !item.value("canDecide", false) || field(item, "status") == "expired") continue;
            const auto kind = field(item, "itemKind");
            inbox.push_back({{"id", item.at("itemId")}, {"botId", id}, {"botName", names.at(id)}, {"kind", kind}});
        }
        for (auto& event : page.events) event["botName"] = names.at(field(event, "botId"));
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
            withConversationAccess(person, view.bot, Policy{store_}.redactSecrets(view.bot, web::botView(store_, bot, nullptr, &person)));
        if (stamp && stamp == Notifications(store_).botViewStamp())
            return {std::move(data), ChangeFeed::BotVersion{*stamp, std::move(bot)}};
        return data;
    }

    Json readBotView(const Person& person, const std::string& id, const Json& bot, const std::vector<TaskView>* changedSessionTasks) {
        Policy policy{store_};
        if (const auto cached = changes_.cachedBotView(person, id, bot)) {
            auto view = cached->data;
            if (changedSessionTasks) {
                view["sessionTasks"] = policy.redactSecrets(id, web::sessionGroups(*changedSessionTasks));
                view.erase("sessionsBefore");
            }
            if (Notifications(store_).botViewStamp() == cached->botVersion->stamp) return view;
        }
        return policy.redactSecrets(id, web::botView(store_, bot, changedSessionTasks, &person));
    }

    Json botList(const Person& person) const {
        auto visible = bots();
        Policy policy{store_};
        visible.erase(std::remove_if(visible.begin(), visible.end(),
                                     [&](const Json& bot) { return !policy.allows(person, Action::bot_view, field(bot, "id")); }),
                      visible.end());
        auto list = web::botList(store_, visible);
        if (list.empty()) return list;
        for (auto& bot : list) {
            const auto id = field(bot, "id");
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
        }
        return list;
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
                if (previous) return reply(res, 201, *previous);
                const bool identityRequired = web::hasWebIdentities(callbacks_);
                if (identityRequired && !callbacks_.resolveIdentity) return error(res, 503, "message_identity_unavailable");
                Intake intake;
                try {
                    intake = callbacks_.intake(bot, request);
                } catch (const BotProfileError& e) {
                    if (e.code == "removed_bot_setting") return error(res, 400, e.code, e.what());
                } catch (const std::exception&) {
                }
                // The caller supplies only a Bot and text. Routing and config come from the callback.
                intake.botId = bot;
                intake.request = request;
                const auto receipt =
                    identityRequired
                        ? WebConversationIntake(store_).receiveWeb(actor, intake, clientMessageId, request, replyTo, attachments)
                        : ConversationIntake(store_).receive(actor, intake, clientMessageId, request, replyTo, attachments);
                return reply(res, 201, receipt);
            }
            const auto operation = requiredText(body, "operation", 32);
            const auto id = bodyId(body, "taskId");
            const auto wait = bodyId(body, "waitId", operation == "cancel" ? 0 : 1);
            auto data = operationData(body, operation);
            const auto task = Tasks(store_).task(id);
            if (task.task.botId != bot) return error(res, 404, "task_not_found");
            TaskControl(store_).operate(actor, {bot, id, wait, bodyId(body, "targetRevision", 0), bodyId(body, "targetGeneration", 0)},
                                        operation, data);
            reply(res, 200, {{"accepted", true}});
        } catch (const InputError& e) {
            error(res, 400, e.what());
        } catch (const AccessDenied&) {
            error(res, 403, "access_denied");
        } catch (const BotPaused& e) {
            error(res, 409, "bot_paused", e.what());
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

namespace {

std::string authority(const std::string& address, unsigned port) {
    const auto host = address.find(':') == address.npos ? address : "[" + address + "]";
    return host + ":" + std::to_string(port);
}

class RemoteSessionMcp final : public WebServer {
public:
    RemoteSessionMcp(Store& store, const std::string& address, unsigned port, WebCallbacks callbacks)
        : callbacks_(std::move(callbacks)), port_(port) {
        server_.new_task_queue = [] { return new httplib::ThreadPool(2, 4, 16); };
        server_.set_address_family(AF_UNSPEC)
            .set_payload_max_length(64 * 1024)
            .set_read_timeout(10)
            .set_write_timeout(10)
            .set_keep_alive_timeout(1)
            .set_default_headers({{"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"}});
        server_.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
            const auto reject = [&](int status) {
                res.status = status;
                return httplib::Server::HandlerResponse::Handled;
            };
            if (req.has_header("Origin") || req.has_header("Sec-Fetch-Site")) return reject(403);
            if (req.path != web::sessionMcpPath) return reject(404);
            return httplib::Server::HandlerResponse::Unhandled;
        });
        web::sessionMcpRoute(server_, store, callbacks_);
        const auto bindError = "Could not listen for other computers at " + authority(address, port);
        if (!server_.bind_to_port(address, static_cast<int>(port))) {
            server_.stop();
            throw std::runtime_error(bindError);
        }
        worker_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        if (!server_.is_running()) {
            worker_.join();
            throw std::runtime_error(bindError);
        }
    }

    ~RemoteSessionMcp() override {
        server_.stop();
        if (worker_.joinable()) worker_.join();
    }

    unsigned port() const override { return port_; }
private:
    WebCallbacks callbacks_;
    const unsigned port_;
    httplib::Server server_;
    std::thread worker_;
};

}

std::unique_ptr<WebServer> startRemoteSessionMcp(Store& store, const std::string& address, unsigned port, WebCallbacks callbacks) {
    if (address.empty() || port == 0 || port > 65535 || !callbacks.bots || !callbacks.intake)
        throw std::invalid_argument("Invalid remote Session MCP settings");
    return std::make_unique<RemoteSessionMcp>(store, address, port, std::move(callbacks));
}

std::string remoteSessionMcpUrl(const std::string& address, unsigned port) {
    return "http://" + authority(address, port) + web::sessionMcpPath;
}
} // namespace flockv0
