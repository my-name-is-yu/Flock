#include "http.hpp"
#include "flock/limits.hpp"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

namespace flockv0::web {
std::string_view conversationMediaKind(std::string_view mime) {
    if (limits::messages::isAttachmentImage(mime)) return "image";
    constexpr std::array<std::string_view, 17> media{
        "audio/aac", "audio/flac", "audio/mp4", "audio/mpeg", "audio/ogg", "audio/wav", "audio/webm", "audio/x-flac",
        "audio/x-m4a", "audio/x-wav", "video/mp4", "video/mpeg", "video/ogg", "video/quicktime", "video/webm",
        "video/x-m4v", "video/x-quicktime"
    };
    if (std::ranges::find(media, mime) != media.end()) return mime.substr(0, mime.find('/'));
    return {};
}

void reply(httplib::Response& res, int status, const Json& body) {
    res.status = status;
    res.set_content(body.dump(-1, ' ', false, Json::error_handler_t::replace), "application/json; charset=utf-8");
}

void error(httplib::Response& res, int status, std::string_view code) {
    reply(res, status, {{"error", code}});
}

void error(httplib::Response& res, int status, std::string_view code, std::string_view message) {
    reply(res, status, {{"error", code}, {"message", message}});
}
}
