#pragma once

#include "flock/json.hpp"
#include "flock/model_error.hpp"

#include <nlohmann/json.hpp>

#include <sys/types.h>

#include <chrono>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0::app::chatgpt {
inline constexpr std::string_view issuer = "https://auth.openai.com";
inline constexpr std::string_view resource = "https://api.openai.com/v1";
inline constexpr std::string_view directScope = "chatgpt.tokens.use.direct";

struct HttpResponse {
    long status = 0;
    std::string contentType, requestId, body;
};

struct HttpRequest {
    std::string url, body, authorization, contentType, accept;
    std::chrono::milliseconds timeout{15000};
    std::stop_token stop{};
    // Return false once the response is complete. Non-success bodies are buffered.
    std::function<bool(std::string_view, const HttpResponse&)> onData{};
    // Empty uses the system trust configuration.
    std::string caFile{};
};

using HttpTransport = std::function<HttpResponse(const HttpRequest&)>;
using BrowserOpener = std::function<std::optional<pid_t>(const std::string&)>;
using Form = std::vector<std::pair<std::string, std::string>>;

struct Authorization {
    std::string code, clientId, redirectUri;
};

struct Failure : ModelError {
    Failure(std::string message, bool transient = false, std::string providerCode = {}, Json usage = Json::object(),
            ModelErrorCode code = ModelErrorCode::inference_failed);
    std::string providerCode;
};

const Json& field(const Json&, std::string_view);
std::string stringField(const Json&, std::string_view);
Json tokenUsage(const Json&);
Failure remoteFailure(std::string_view site, long status, const Json&, std::string_view requestId = {}, Json usage = Json::object());
bool validAccessToken(std::string_view);

HttpResponse httpRequest(const HttpRequest&);
Json infer(const HttpTransport&, const std::string& token, std::string body, const Json* request = nullptr);
inline constexpr std::string_view rejectedImageCodes[]{"invalid_image",     "invalid_image_format", "invalid_base64_image",
                                                       "image_parse_error", "invalid_image_mode",   "unsupported_image_media_type",
                                                       "image_too_large",   "image_too_small",      "image_file_too_large",
                                                       "empty_image_file"};
Json models(const HttpTransport&, const std::string& token);

std::string formEncode(const Form&);
double nowMillis();
Json discover(const HttpTransport&, std::stop_token stop = {}, std::chrono::milliseconds timeout = std::chrono::seconds(15));
Json exchange(const HttpTransport&, const Json& discovery, Form parameters, std::stop_token stop = {});
Json identity(const HttpTransport&, const Json& idToken, const std::string& clientId, const Json& discovery,
              const std::optional<std::string>& nonce, double now, std::stop_token stop = {});
Json credentials(const Json& tokens, const Json& previous = Json::object());
Json authorize(const HttpTransport&, const BrowserOpener&, const Json& active, bool printUrl, bool reconsent, bool switchAccount, bool web,
               std::stop_token, const std::function<void(const Json&)>& saveRegistration);
bool revokeSession(const HttpTransport&, const std::string& clientId, const std::string& refreshToken);

Authorization authorizeBrowser(const BrowserOpener&, const std::string& endpoint, Form parameters, const std::string& savedClient,
                               bool printUrl, bool browserRequired, std::stop_token);
std::optional<pid_t> openBrowser(const std::string&);
}
