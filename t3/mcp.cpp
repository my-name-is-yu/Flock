#include "internal.hpp"
#include "flock/detail/json_rpc.hpp"

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
            if (key == "www-authenticate" && headers.contains(key))
                headers[key] += ", " + trim(line.substr(end + 1));
            else headers[key] = trim(line.substr(end + 1));
        }
    } catch (...) { return 0; }
    return line.size();
}

// Only a validation diagnostic specific to this property is safe to retry.
bool rejectsDelegatedFrom(std::string_view message) {
    return message == "Unknown field: delegatedFrom" ||
        message == "Unrecognized key: \"delegatedFrom\"" ||
        message == "Unexpected property: delegatedFrom" ||
        message == "Unknown argument: delegatedFrom" ||
        message == "delegatedFrom is not supported" ||
        message.find("[\"delegatedFrom\"] is unexpected") != message.npos;
}

Json rpcResponse(const HttpResponse& response, unsigned id) {
    const auto type = response.headers.find("content-type");
    const std::string_view contentType = type == response.headers.end() ? std::string_view{} : type->second;
    auto message = detail::decodeRpcResponse(response.body, contentType, id, [](std::string_view frame) {
        // T3 ignores SSE events whose data fields are empty.
        if (frame.find_first_not_of('\n') == frame.npos) return Json();
        return parse(frame);
    });
    if (str(message, "jsonrpc") != "2.0") invalidResponse();
    if (message.contains("error")) {
        const auto& error = message["error"];
        if (message.contains("result") || !error.contains("code") || !error["code"].is_number_integer() ||
            !error.contains("message") || !error["message"].is_string()) invalidResponse();
        const auto detail = str(error,"message");
        const auto reason = error["code"] == -32602 && rejectsDelegatedFrom(detail)
            ? Failure::Reason::DelegatedFromRejected : Failure::Reason::Other;
        throw Failure(Failure::Kind::Rpc, "T3 JSON-RPC error " + error["code"].dump() + ": " + detail,reason);
    }
    if (!message.contains("result") || !message["result"].is_object()) invalidResponse();
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
    if (const auto code = curl_easy_perform(curl.get()); code != CURLE_OK)
        throw Failure(Failure::Kind::Wire, "T3 HTTP connection failed or timed out",
                      code == CURLE_OPERATION_TIMEDOUT ? Failure::Reason::Timeout : Failure::Reason::Other);
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}

Client::Client(Credential credential, SessionReadDeadline deadline)
    : credential_(std::move(credential)), deadline_(deadline), headers_{
    {"Authorization", "Bearer " + credential_.token}, {"Accept", "application/json, text/event-stream"},
    {"Content-Type", "application/json"}} {
    try {
        const auto result = rpc("initialize", initializeParams());
        if (str(result, "protocolVersion") != ProtocolVersion) invalidResponse();
        headers_["MCP-Protocol-Version"] = ProtocolVersion;
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
    // Leave time for best-effort MCP session teardown within the same deadline.
    const auto cleanupTime = std::chrono::milliseconds(method == "DELETE" ? 0 : 250);
    const auto remaining = deadline_ ? std::chrono::ceil<std::chrono::milliseconds>(
        *deadline_ - std::chrono::steady_clock::now() - cleanupTime).count() : timeoutMs;
    if (remaining <= 0) throw SessionReadTimeout();
    HttpResponse response;
    try {
        response = http(credential_.mcpUrl, method, headers_, body, std::min<long>(timeoutMs,remaining));
    } catch (const Failure& error) {
        if (deadline_ && remaining <= timeoutMs && error.reason == Failure::Reason::Timeout)
            throw SessionReadTimeout();
        throw;
    }
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

// T3 v0.1.11 uses a shared invalid_request code for several distinct outcomes.
static Failure toolFailure(const Json& payload) {
    const auto& error = payload.contains("error") && payload["error"].is_object() ? payload["error"] : payload;
    const auto code = str(error, "code");
    const auto message = str(error, "message");
    if ((code == "invalid_request" || code == "invalid_params") && rejectsDelegatedFrom(message))
        return Failure(Failure::Kind::Tool,"T3 rejected the delegatedFrom launch field",Failure::Reason::DelegatedFromRejected);
    if (code == "capability_denied" || code == "thread_credential_required" ||
        code == "runtime_mode_escalation_denied" || code == "interaction_mode_escalation_denied")
        return Failure(Failure::Kind::Auth, "T3 OAuth approval does not allow this operation or mode");
    if (code == "thread_not_found" || code == "run_not_found" || code == "item_not_found" || code == "attachment_not_found" ||
        (code == "invalid_request" && message == "The thread was not found."))
        return Failure(Failure::Kind::Tool, "T3 history was not found", Failure::Reason::HistoryNotFound);
    if (code == "project_not_found" || (code == "invalid_request" && message == "The project was not found."))
        return Failure(Failure::Kind::Settings, "T3 rejected the project, model, or workspace settings",
                       Failure::Reason::ProjectNotFound);
    if (code == "model_unavailable" || code == "provider_unavailable" ||
        (code == "invalid_request" && (message.starts_with("Pass modelSelection:") ||
         message.find("No thread was created.") != std::string::npos)))
        return Failure(Failure::Kind::Settings, "T3 rejected the project, model, or workspace settings",
                       Failure::Reason::WorkSettings);
    if (code == "invalid_request" && (message == "The pending user-input request was not found." ||
        message == "The user-input request was already answered." ||
        message.starts_with("The user-input request is no longer pending (")))
        return Failure(Failure::Kind::Tool, message, Failure::Reason::QuestionGone);
    return Failure(Failure::Kind::Tool, "T3 rejected the operation");
}

Json Client::call(std::string_view tool, Json arguments) {
    auto response = rpc("tools/call", {{"name", tool}, {"arguments", std::move(arguments)}});
    if (response.contains("isError") && !response["isError"].is_boolean()) invalidResponse();
    const bool isError = response.value("isError", false);
    Json data = response.value("structuredContent", Json());
    if (data.is_null()) {
        const auto content = response.find("content");
        if (content != response.end() && content->is_array() && content->size() == 1 &&
            str(content->front(), "type") == "text") {
            const auto detail = str(content->front(),"text");
            if (isError && detail.starts_with("MCP error -32602: Input validation error:") && rejectsDelegatedFrom(detail))
                throw Failure(Failure::Kind::Tool,"T3 rejected the delegatedFrom launch field",Failure::Reason::DelegatedFromRejected);
            data = Json::parse(detail,nullptr,false);
        }
    }
    if (isError) throw toolFailure(data);
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
