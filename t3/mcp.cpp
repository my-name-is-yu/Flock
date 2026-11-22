#include "internal.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <memory>

namespace flockv0::t3detail {
namespace {
using Easy = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using UrlHandle = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
constexpr std::size_t MaxResponseBytes = 2 * 1024 * 1024;

void initializeCurl() {
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) throw Failure(Failure::Kind::Wire, "Could not initialize T3 HTTP transport");
}

std::string part(CURLU* url, CURLUPart key, unsigned flags = 0) {
    char* value = nullptr;
    if (curl_url_get(url, key, &value, flags) != CURLUE_OK) return {};
    const std::unique_ptr<char, decltype(&curl_free)> buffer(value, curl_free);
    return buffer.get();
}

size_t receiveBody(char* data, size_t size, size_t count, void* context) noexcept {
    auto& body = static_cast<HttpResponse*>(context)->body;
    const auto length = size * count;
    if (length > MaxResponseBytes - body.size()) return 0;
    try { body.append(data, length); }
    catch (...) { return 0; }
    return length;
}

size_t receiveHeader(char* data, size_t size, size_t count, void* context) noexcept {
    auto& headers = static_cast<HttpResponse*>(context)->headers;
    const std::string_view line(data, size * count);
    try {
        if (line.starts_with("HTTP/")) headers.clear();
        const auto end = line.find(':');
        if (end != std::string_view::npos) {
            auto key = std::string(line.substr(0, end));
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
            headers[std::move(key)] = trim(line.substr(end + 1));
        }
    } catch (...) { return 0; }
    return line.size();
}

Json rpcResponse(const HttpResponse& response, unsigned id) {
    const auto type = response.headers.find("content-type");
    Json message;
    if (type == response.headers.end() || !type->second.starts_with("text/event-stream")) {
        message = parse(response.body);
    } else {
        std::string event;
        std::string_view tail = response.body;
        while (!tail.empty()) {
            const auto end = tail.find('\n');
            auto line = tail.substr(0, end);
            tail.remove_prefix(end == std::string_view::npos ? tail.size() : end + 1);
            if (line.ends_with('\r')) line.remove_suffix(1);
            if (line.starts_with("data:")) {
                line.remove_prefix(5);
                if (line.starts_with(' ')) line.remove_prefix(1);
                if (!event.empty()) event += '\n';
                event += line;
            } else if (line.empty() && !event.empty()) {
                auto candidate = parse(event);
                if (candidate.is_object() && candidate.contains("id") && candidate["id"] == id) {
                    message = std::move(candidate);
                    break;
                }
                event.clear();
            }
        }
    }
    if (str(message, "jsonrpc") != "2.0" || !message.contains("id") || message["id"] != id ||
        !message.contains("result") || !message["result"].is_object() || message.contains("error"))
        invalidResponse();
    return std::move(message["result"]);
}
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return std::string(value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1));
}

std::string str(const Json& object, std::string_view name) {
    const auto it = object.find(name);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

Json parse(std::string_view body) {
    auto value = Json::parse(body, nullptr, false);
    if (value.is_discarded()) invalidResponse();
    return value;
}

void invalidResponse() { throw Failure(Failure::Kind::Wire, "T3 returned an incomplete or unexpected response"); }

Url checkedUrl(const std::string& value) {
    initializeCurl();
    UrlHandle url(curl_url(), curl_url_cleanup);
    const auto invalid = [] { throw Failure(Failure::Kind::Settings, "T3 URLs must use HTTPS or a loopback HTTP address"); };
    if (!url || value.size() > 8192 || value.find_first_of("?#\r\n\t ") != std::string::npos ||
        curl_url_set(url.get(), CURLUPART_URL, value.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK)
        invalid();
    const auto scheme = part(url.get(), CURLUPART_SCHEME);
    const auto host = part(url.get(), CURLUPART_HOST);
    const auto port = part(url.get(), CURLUPART_PORT, CURLU_NO_DEFAULT_PORT);
    if (host.empty() || (scheme != "https" && (scheme != "http" ||
        (host != "127.0.0.1" && host != "localhost" && host != "[::1]")))) invalid();
    const auto origin = scheme + "://" + host + (port.empty() ? "" : ":" + port);
    const auto path = part(url.get(), CURLUPART_PATH);
    return {origin + path, origin, path};
}

std::string encode(std::string_view value) {
    std::string result;
    const auto hex = "0123456789ABCDEF";
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') result += static_cast<char>(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}

std::string decode(std::string_view value) {
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = value[i];
        if (c != '%') { result += c == '+' ? ' ' : c; continue; }
        if (i + 2 >= value.size()) invalidResponse();
        const int high = nibble(value[i + 1]), low = nibble(value[i + 2]);
        if (high < 0 || low < 0) invalidResponse();
        result += static_cast<char>((high << 4) | low);
        i += 2;
    }
    return result;
}

HttpResponse http(const std::string& url, std::string_view method, const Headers& headers,
                  const std::string& body, long timeoutMs) {
    initializeCurl();
    Easy curl(curl_easy_init(), curl_easy_cleanup);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> list(nullptr, curl_slist_free_all);
    if (!curl) throw Failure(Failure::Kind::Wire, "Could not initialize T3 HTTP request");
    HttpResponse response;
    const std::string methodName(method);
    for (const auto& [key, value] : headers) {
        if (value.find_first_of("\r\n\0", 0, 3) != std::string::npos) invalidResponse();
        auto* appended = curl_slist_append(list.get(), (key + ": " + value).c_str());
        if (!appended) invalidResponse();
        list.release();
        list.reset(appended);
    }
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, std::min(2500L, timeoutMs));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "127.0.0.1,localhost,::1");
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, methodName.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, list.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receiveBody);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, receiveHeader);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response);
    if (method == "POST") {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    if (curl_easy_perform(curl.get()) != CURLE_OK)
        throw Failure(Failure::Kind::Wire, "T3 HTTP connection failed or timed out");
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}

Client::Client(Credential credential) : credential_(std::move(credential)), headers_{
    {"Authorization", "Bearer " + credential_.token}, {"Accept", "application/json, text/event-stream"},
    {"Content-Type", "application/json"}} {
    constexpr auto version = "2025-06-18";
    try {
        const auto result = rpc("initialize", {{"protocolVersion", version}, {"capabilities", Json::object()},
                                              {"clientInfo", {{"name", "Flock"}, {"version", "0.1.0"}}}});
        if (str(result, "protocolVersion") != version) invalidResponse();
        headers_["MCP-Protocol-Version"] = version;
        request("POST", Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump());
    } catch (...) {
        if (headers_.contains("Mcp-Session-Id")) {
            try { request("DELETE", {}, 750); } catch (...) {}
        }
        throw;
    }
}

Client::~Client() {
    if (headers_.contains("Mcp-Session-Id")) {
        try { request("DELETE", {}, 750); } catch (...) {}
    }
}

HttpResponse Client::request(std::string_view method, const std::string& body, long timeoutMs) {
    auto response = http(credential_.mcpUrl, method, headers_, body, timeoutMs);
    if (response.status == 401 || response.status == 403)
        throw Failure(Failure::Kind::Auth, "T3 refused the OAuth credential or its access level");
    if (response.status < 200 || response.status >= 300)
        throw Failure(Failure::Kind::Wire, "T3 returned an unsuccessful HTTP response");
    if (const auto session = response.headers.find("mcp-session-id"); session != response.headers.end()) {
        if (session->second.empty() || session->second.size() > 1024) invalidResponse();
        headers_["Mcp-Session-Id"] = session->second;
    }
    return response;
}

Json Client::rpc(std::string_view method, const Json& arguments) {
    const auto id = ++nextId_;
    const auto body = Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", arguments}}.dump();
    return rpcResponse(request("POST", body), id);
}

Json Client::call(std::string_view tool, Json arguments) {
    auto response = rpc("tools/call", {{"name", tool}, {"arguments", std::move(arguments)}});
    const bool isError = response.value("isError", false);
    Json data = response.value("structuredContent", Json());
    if (data.is_null()) {
        const auto content = response.find("content");
        if (content != response.end() && content->is_array() && content->size() == 1 &&
            str(content->front(), "type") == "text") data = parse(str(content->front(), "text"));
    }
    if (data.contains("error") && data["error"].is_object()) data = Json(data["error"]);
    const auto code = str(data, "code");
    if (isError || !code.empty()) {
        const bool denied = code == "capability_denied" || code == "thread_credential_required" ||
                            code == "runtime_mode_escalation_denied" || code == "interaction_mode_escalation_denied";
        throw Failure(denied ? Failure::Kind::Auth : Failure::Kind::Tool,
                      denied ? "T3 OAuth approval does not allow this operation or mode" : "T3 rejected the operation",
                      code, str(data, "message"));
    }
    if (!data.is_object()) invalidResponse();
    return data;
}

Json readThread(Client& client, std::string_view threadId, std::string_view view, unsigned limit, int chars,
                Json args, int runLimit) {
    args.update(Json{{"threadId",threadId},{"view",view},{"limit",limit},{"maxCharsPerItem",chars},{"runLimit",runLimit}});
    auto page = client.call("t3_thread_read",std::move(args));
    if (str(member(page,"thread"),"threadId") != threadId || array(page,"items").size() > limit)
        invalidResponse();
    return page;
}

void advancePosition(const Json& item, Id& after) {
    const auto next = position(member(item,"position"));
    if (next <= after) invalidResponse();
    after = next;
}

bool moreHistory(const Json& page, Id after, Id start) {
    if (!boolean(page,"hasMore")) return false;
    if (after <= start || position(member(page,"nextPosition")) != after) invalidResponse();
    return true;
}
}
