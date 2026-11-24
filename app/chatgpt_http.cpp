#include "chatgpt.hpp"

#include <curl/curl.h>
#include <sys/socket.h>

#ifndef SO_NOSIGPIPE
#include <pthread.h>
#include <signal.h>
#endif

#include <algorithm>
#include <cerrno>
#include <exception>

namespace flockv0::app::chatgpt {
namespace {
constexpr std::size_t maxResponse = 4 * 1024 * 1024;
constexpr std::string_view responsesApi = "ChatGPT Responses API";

bool success(long status) { return status >= 200 && status < 300; }

std::string safeCode(const Json& value) {
    constexpr std::string_view characters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-";
    if (!value.is_string()) return {};
    const auto& code = value.get_ref<const std::string&>();
    return code.size() <= 100 && code.find_first_not_of(characters) == code.npos ? code : std::string{};
}

#if defined(SO_NOSIGPIPE)
int suppressSocketSigpipe(void*, curl_socket_t socket, curlsocktype) {
    // On macOS a broken socket can signal the process instead of its writer.
    const int enabled = 1;
    return setsockopt(socket,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled)) == 0
        ? CURL_SOCKOPT_OK : CURL_SOCKOPT_ERROR;
}
#else
class BlockSigpipe {
public:
    BlockSigpipe() {
        sigemptyset(&blocked_);
        sigaddset(&blocked_,SIGPIPE);
        if (pthread_sigmask(SIG_BLOCK,&blocked_,&previous_) != 0) fail();
        sigset_t pending;
        if (sigpending(&pending) != 0) {
            pthread_sigmask(SIG_SETMASK,&previous_,nullptr);
            fail();
        }
        alreadyPending_ = sigismember(&pending,SIGPIPE) == 1;
    }

    ~BlockSigpipe() {
        // Writes to closed sockets signal the writing thread. Consume any
        // SIGPIPE from this transfer before restoring the caller's mask.
        if (!alreadyPending_) {
            const timespec noWait{};
            while (sigtimedwait(&blocked_,nullptr,&noWait) == -1 && errno == EINTR) {}
        }
        pthread_sigmask(SIG_SETMASK,&previous_,nullptr);
    }

    BlockSigpipe(const BlockSigpipe&) = delete;
    BlockSigpipe& operator=(const BlockSigpipe&) = delete;

private:
    [[noreturn]] static void fail() { throw ModelError("Unable to protect ChatGPT transport from broken pipes",true); }
    sigset_t blocked_, previous_;
    bool alreadyPending_;
};
#endif

struct Transfer {
    CURL* curl;
    const HttpRequest& request;
    HttpResponse response{};
    std::size_t received = 0;
    bool complete = false;
    std::exception_ptr failure{};

    void headers() {
        char* contentType = nullptr;
        curl_header* requestId = nullptr;
        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&response.status);
        if (curl_easy_getinfo(curl,CURLINFO_CONTENT_TYPE,&contentType) == CURLE_OK && contentType)
            response.contentType = contentType;
        if (curl_easy_header(curl,"x-request-id",0,CURLH_HEADER,-1,&requestId) == CURLHE_OK)
            response.requestId = requestId->value;
    }
};

std::size_t receive(char* bytes, std::size_t size, std::size_t count, void* context) noexcept {
    auto& transfer = *static_cast<Transfer*>(context);
    const std::string_view chunk(bytes,size * count);
    try {
        if (chunk.size() > maxResponse - transfer.received)
            throw ModelError("ChatGPT response exceeded the transport limit",false);
        if (transfer.received == 0) transfer.headers();
        transfer.received += chunk.size();
        if (transfer.request.onData && success(transfer.response.status)) {
            transfer.complete = !transfer.request.onData(chunk,transfer.response);
            if (transfer.complete) return 0;
        } else {
            transfer.response.body.append(chunk);
        }
        return chunk.size();
    } catch (...) {
        transfer.failure = std::current_exception();
        return 0;
    }
}

int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<const Transfer*>(context)->request.stop.stop_requested() ? 1 : 0;
}

bool eventStream(std::string_view contentType) {
    contentType = contentType.substr(0,contentType.find(';'));
    const auto begin = contentType.find_first_not_of(" \t");
    if (begin == contentType.npos) return true;
    contentType = contentType.substr(begin,contentType.find_last_not_of(" \t") - begin + 1);
    constexpr std::string_view expected = "text/event-stream";
    return std::ranges::equal(contentType,expected,[](unsigned char a, unsigned char b) {
        return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == b;
    });
}

// Validate the entire stream, including comments, across arbitrary curl chunk boundaries.
class Utf8 {
public:
    void read(unsigned char c) {
        if (remaining_ != 0) {
            if (c < minimum_ || c > maximum_) invalid();
            --remaining_;
            minimum_ = 0x80;
            maximum_ = 0xbf;
        } else if (c >= 0x80) {
            if (c < 0xc2 || c > 0xf4) invalid();
            remaining_ = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
            minimum_ = c == 0xe0 ? 0xa0 : c == 0xf0 ? 0x90 : 0x80;
            maximum_ = c == 0xed ? 0x9f : c == 0xf4 ? 0x8f : 0xbf;
        }
    }
    void finish() const { if (remaining_ != 0) invalid(); }
private:
    [[noreturn]] static void invalid() { throw ModelError("Invalid UTF-8 in ChatGPT response stream",false); }
    int remaining_ = 0;
    unsigned char minimum_ = 0x80, maximum_ = 0xbf;
};

class ResponseStream {
public:
    bool read(std::string_view chunk, const HttpResponse& response) {
        if (!eventStream(response.contentType))
            throw ModelError("ChatGPT did not return the expected response stream",false);
        for (const auto c : chunk) {
            utf8_.read(static_cast<unsigned char>(c));
            // SSE accepts CR, LF, and CRLF, including a separator split across chunks.
            if (c == '\n' && afterCr_) {
                afterCr_ = false;
                continue;
            }
            afterCr_ = c == '\r';
            if (c == '\r' || c == '\n') {
                if (!line(response.requestId)) return false;
                line_.clear();
            } else {
                line_.push_back(c);
            }
        }
        return true;
    }

    Json take() {
        utf8_.finish();
        if (completed_.is_null()) throw ModelError("ChatGPT stream ended before a completed response",true);
        return std::move(completed_);
    }

private:
    bool line(std::string_view requestId) {
        std::string_view current(line_);
        if (firstLine_) {
            firstLine_ = false;
            if (current.starts_with("\xef\xbb\xbf")) current.remove_prefix(3);
        }
        if (!current.empty()) {
            const auto colon = current.find(':');
            if (current.substr(0,colon) == "data") {
                if (colon != current.npos) current.remove_prefix(colon + 1);
                else current = {};
                if (current.starts_with(' ')) current.remove_prefix(1);
                data_.append(current);
                data_.push_back('\n');
            }
            return true;
        }
        if (data_.empty()) return true;
        data_.pop_back();
        if (data_ == "[DONE]") {
            data_.clear();
            return true;
        }
        const auto event = Json::parse(data_,nullptr,false);
        data_.clear();
        if (event.is_discarded()) throw ModelError("Invalid JSON in ChatGPT response stream",false);
        const auto type = stringField(event,"type");
        if (type.empty()) throw ModelError("Invalid ChatGPT response event",false);
        const auto& result = field(event,"response");
        if (type == "response.completed") {
            if (!field(result,"output").is_array()) throw ModelError("Invalid ChatGPT completion",false);
            completed_ = result;
            return false;
        }
        const auto& responseUsage = field(result,"usage");
        const auto& usage = responseUsage.is_null() ? field(event,"usage") : responseUsage;
        if (type == "response.incomplete")
            throw Failure("ChatGPT returned an incomplete response",false,{},usage);
        if (type == "response.failed" || type == "error")
            throw remoteFailure(responsesApi,0,result.is_null() ? event : result,requestId,usage);
        return true;
    }

    Utf8 utf8_;
    std::string line_, data_;
    bool firstLine_ = true, afterCr_ = false;
    Json completed_ = nullptr;
};

void checkApi(const HttpResponse& response) {
    if (!success(response.status))
        throw remoteFailure(responsesApi,response.status,Json::parse(response.body,nullptr,false),response.requestId);
}
}

const Json& field(const Json& object, std::string_view name) {
    static const Json missing;
    const auto it = object.find(name);
    return it != object.end() ? *it : missing;
}

std::string stringField(const Json& object, std::string_view name) {
    const auto& value = field(object,name);
    return value.is_string() ? value.get<std::string>() : std::string{};
}

Json tokenUsage(const Json& usage) {
    auto result = Json::object();
    for (const auto* key : {"input_tokens","output_tokens","total_tokens"}) {
        const auto& count = field(usage,key);
        if (count.is_number_integer() && count.get<long double>() >= 0) result[key] = count;
    }
    return result;
}

Failure::Failure(std::string message, bool transient, std::string code, Json usage)
    : ModelError(std::move(message),transient,tokenUsage(usage)), providerCode(std::move(code)) {}

Failure remoteFailure(std::string_view site, long status, const Json& value, std::string_view requestId, Json usage) {
    const auto& error = field(value,"error");
    const auto& errorCode = field(error,"code");
    const auto& errorParameter = field(error,"param");
    const auto code = safeCode(error.is_string() ? error : errorCode.is_null() ? field(value,"code") : errorCode);
    const auto parameter = safeCode(errorParameter.is_null() ? field(value,"param") : errorParameter);
    const auto safeId = safeCode(Json(requestId));
    std::string details;
    const auto add = [&](std::string part) {
        if (!details.empty()) details += ", ";
        details += part;
    };
    if (status != 0) add("HTTP " + std::to_string(status));
    if (!code.empty()) add(code);
    if (!parameter.empty()) add("parameter " + parameter);
    if (!safeId.empty()) add("request " + safeId);
    std::string message(site);
    message += " rejected the request";
    if (!details.empty()) message += " (" + details + ")";
    const bool limit = code == "subscription_sharing_usage_limit_exceeded";
    if (limit) message += " — review the app limit at https://chatgpt.com/settings/usage";
    else if (site == responsesApi && status == 401)
        message += " — check Flock's ChatGPT connection and granted plan permissions";
    const bool transient = !limit && (status == 429 || status == 408 || (status >= 500 && status <= 599) ||
        code == "server_error" || code == "rate_limit_exceeded" || code == "service_unavailable");
    return Failure(std::move(message),transient,code,std::move(usage));
}

bool validAccessToken(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value,[](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}

HttpResponse httpRequest(const HttpRequest& request) {
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) throw ModelError("Unable to initialize ChatGPT transport",true);
    if (request.stop.stop_requested()) throw ModelError("ChatGPT sign-in was cancelled",true);
#ifndef SO_NOSIGPIPE
    // The mask must stay in place through TLS cleanup.
    const BlockSigpipe blockSigpipe;
#endif
    const std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
    if (!curl) throw ModelError("Unable to initialize ChatGPT transport",true);
    std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> headers(nullptr,curl_slist_free_all);
    const auto add = [&](const std::string& header) {
        auto* list = curl_slist_append(headers.get(),header.c_str());
        if (!list) throw ModelError("Unable to initialize ChatGPT transport headers",true);
        headers.release();
        headers.reset(list);
    };
    if (!request.authorization.empty()) add("Authorization: " + request.authorization);
    if (!request.contentType.empty()) add("Content-Type: " + request.contentType);
    if (!request.accept.empty()) add("Accept: " + request.accept);
    // Send small and large requests the same way.
    add("Expect:");

    Transfer transfer{curl.get(),request};
    const auto set = [&](CURLoption option, auto value) {
        if (curl_easy_setopt(curl.get(),option,value) != CURLE_OK)
            throw ModelError("Unable to configure ChatGPT transport",true);
    };
    set(CURLOPT_URL,request.url.c_str());
    set(CURLOPT_PROTOCOLS_STR,"https");
    set(CURLOPT_DISALLOW_USERNAME_IN_URL,1L);
    set(CURLOPT_FOLLOWLOCATION,0L);
    set(CURLOPT_SSL_VERIFYPEER,1L);
    set(CURLOPT_SSL_VERIFYHOST,2L);
    if (!request.caFile.empty()) set(CURLOPT_CAINFO,request.caFile.c_str());
    set(CURLOPT_NOSIGNAL,1L);
#ifdef SO_NOSIGPIPE
    set(CURLOPT_SOCKOPTFUNCTION,suppressSocketSigpipe);
#endif
    set(CURLOPT_TIMEOUT_MS,static_cast<long>(request.timeout.count()));
    set(CURLOPT_ACCEPT_ENCODING,"");
    set(CURLOPT_HTTPHEADER,headers.get());
    set(CURLOPT_WRITEFUNCTION,receive);
    set(CURLOPT_WRITEDATA,&transfer);
    if (request.stop.stop_possible()) {
        set(CURLOPT_NOPROGRESS,0L);
        set(CURLOPT_XFERINFOFUNCTION,progress);
        set(CURLOPT_XFERINFODATA,&transfer);
    }
    if (!request.contentType.empty()) {
        set(CURLOPT_POSTFIELDSIZE_LARGE,static_cast<curl_off_t>(request.body.size()));
        set(CURLOPT_POSTFIELDS,request.body.data());
    }
    const auto result = curl_easy_perform(curl.get());
    if (transfer.failure) std::rethrow_exception(transfer.failure);
    if (request.stop.stop_requested()) throw ModelError("ChatGPT sign-in was cancelled",true);
    if (result != CURLE_OK && !transfer.complete) {
        if (request.onData && transfer.received != 0 && success(transfer.response.status))
            throw ModelError("ChatGPT response stream was interrupted",true);
        throw ModelError("Unable to reach ChatGPT within the request deadline",true);
    }
    if (transfer.received == 0) transfer.headers();
    return std::move(transfer.response);
}

Json models(const HttpTransport& transport, const std::string& token) {
    const auto response = transport({.url = std::string(resource) + "/models", .body = {},
                                     .authorization = "Bearer " + token, .contentType = {}, .accept = "application/json"});
    checkApi(response);
    const auto data = Json::parse(response.body,nullptr,false);
    const auto& catalog = field(data,"models");
    if (!catalog.is_array()) throw ModelError("ChatGPT did not return a model catalog",true);
    auto result = Json::array();
    for (const auto& model : catalog) {
        const auto& slug = field(model,"slug");
        if (field(model,"visibility") != "list" || !slug.is_string()) continue;
        const auto& displayName = field(model,"display_name");
        result.push_back({{"slug",slug},{"displayName",displayName.is_null() ? slug : displayName}});
    }
    return result;
}

Json infer(const HttpTransport& transport, const std::string& token, std::string body) {
    ResponseStream stream;
    const auto response = transport({.url = std::string(resource) + "/responses", .body = std::move(body),
        .authorization = "Bearer " + token, .contentType = "application/json", .accept = "text/event-stream",
        .timeout = std::chrono::seconds(90),
        .onData = [&](std::string_view data, const HttpResponse& headers) { return stream.read(data,headers); }});
    checkApi(response);
    if (!eventStream(response.contentType))
        throw ModelError("ChatGPT did not return the expected response stream",false);
    return stream.take();
}
}
