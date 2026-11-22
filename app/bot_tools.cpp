#include "bot_tools.hpp"
#include "flock/access.hpp"
#include "flock/secrets.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <curl/curl.h>
#include <fstream>
#include <mutex>
#include <sys/socket.h>

namespace flockv0::app {
namespace {
constexpr std::size_t responseLimit = 128 * 1024;
constexpr std::string_view protocol = "2025-03-26";

struct Response {
    std::string body, type, session;
    long status = 0;
    bool truncated = false;
};

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \r\n\t");
    return first == value.npos ? "" : std::string(value.substr(first,value.find_last_not_of(" \r\n\t")-first+1));
}

size_t writeBody(char* data, size_t size, size_t count, void* user) noexcept {
    auto& response = *static_cast<Response*>(user);
    const auto bytes = size*count;
    const auto available = std::min(responseLimit-response.body.size(),bytes);
    try { response.body.append(data,available); }
    catch (...) { return 0; }
    response.truncated = available < bytes;
    return available;
}

size_t writeHeader(char* data, size_t size, size_t count, void* user) noexcept {
    auto& response = *static_cast<Response*>(user);
    const std::string_view line(data,size*count);
    try {
        const auto colon = line.find(':');
        if (colon == line.npos) return line.size();
        std::string key(line.substr(0,colon));
        std::ranges::transform(key,key.begin(),[](unsigned char c) { return std::tolower(c); });
        if (key == "mcp-session-id") response.session = trim(line.substr(colon+1));
        if (key == "content-type") response.type = trim(line.substr(colon+1));
    } catch (...) { return 0; }
    return line.size();
}

bool publicIpv4(std::uint32_t address) {
    const auto a = address >> 24, b = (address >> 16) & 255, c = (address >> 8) & 255;
    return a != 0 && a != 10 && a != 127 && a < 224 &&
        !(a == 100 && b >= 64 && b <= 127) && !(a == 169 && b == 254) &&
        !(a == 172 && b >= 16 && b <= 31) && !(a == 192 && (b == 168 || (b == 0 && (c == 0 || c == 2)))) &&
        !(a == 192 && b == 88 && c == 99) && !(a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) &&
        !(a == 203 && b == 0 && c == 113);
}

curl_socket_t publicSocket(void*, curlsocktype, struct curl_sockaddr* address) {
    if (address->family != AF_INET || address->addrlen < sizeof(sockaddr_in)) return CURL_SOCKET_BAD;
    const auto* peer = reinterpret_cast<const sockaddr_in*>(&address->addr);
    if (!publicIpv4(ntohl(peer->sin_addr.s_addr)) || ntohs(peer->sin_port) != 443) return CURL_SOCKET_BAD;
    return socket(address->family,address->socktype,address->protocol);
}

bool safeHeaderValue(std::string_view value) {
    return !value.empty() && value.size() <= 4096 &&
           std::ranges::all_of(value,[](unsigned char c) { return c >= 33 && c <= 126; });
}

std::string environmentToken(const std::string& name) {
    const char* raw = std::getenv(name.c_str());
    if (!raw || !*raw) return {};
    if (!safeHeaderValue(raw)) throw Conflict("External adapter has an invalid credential");
    return raw;
}

Response http(const std::string& url, bool publicOnly, const std::vector<std::string>& headers,
              const Json& rpc = nullptr, bool allowTruncated = false, bool closeSession = false) {
    static const auto init = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (init != CURLE_OK) throw Conflict("External HTTP transport is unavailable");
    const std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
    if (!curl) throw Conflict("External HTTP transport is unavailable");
    struct Headers {
        curl_slist* list = nullptr;
        ~Headers() { curl_slist_free_all(list); }
    } list;
    for (const auto& header : headers) {
        auto* appended = curl_slist_append(list.list,header.c_str());
        if (!appended) throw Conflict("Cannot allocate external request headers");
        list.list = appended;
    }
    auto set = [&](CURLoption key, auto value) {
        if (curl_easy_setopt(curl.get(),key,value) != CURLE_OK) throw Conflict("Cannot configure external request");
    };
    set(CURLOPT_URL,url.c_str());
    set(CURLOPT_PROTOCOLS_STR,publicOnly ? "https" : "http,https");
    set(CURLOPT_PROXY,"");
    set(CURLOPT_FOLLOWLOCATION,0L);
    set(CURLOPT_NOSIGNAL,1L);
    const long timeout = closeSession ? 750L : 4000L;
    set(CURLOPT_CONNECTTIMEOUT_MS,std::min(2000L,timeout));
    set(CURLOPT_TIMEOUT_MS,timeout);
    set(CURLOPT_HTTPHEADER,list.list);
    set(CURLOPT_USERAGENT,"Flock/0.1");
    if (closeSession) set(CURLOPT_CUSTOMREQUEST,"DELETE");
    set(CURLOPT_NETRC,static_cast<long>(CURL_NETRC_IGNORED));
    if (publicOnly) {
        set(CURLOPT_IPRESOLVE,static_cast<long>(CURL_IPRESOLVE_V4));
        set(CURLOPT_OPENSOCKETFUNCTION,publicSocket);
    }
    Response response;
    set(CURLOPT_WRITEFUNCTION,writeBody);
    set(CURLOPT_WRITEDATA,&response);
    set(CURLOPT_HEADERFUNCTION,writeHeader);
    set(CURLOPT_HEADERDATA,&response);
    std::string body;
    if (!rpc.is_null()) {
        body = rpc.dump();
        set(CURLOPT_POSTFIELDS,body.c_str());
    }
    const auto result = curl_easy_perform(curl.get());
    curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&response.status);
    const bool rpcRejection = rpc.contains("id") && response.status >= 400 && response.status < 500;
    const bool success = response.status >= 200 && response.status < 300;
    if ((!success && !rpcRejection) ||
        (result != CURLE_OK && !(result == CURLE_WRITE_ERROR && response.truncated && allowTruncated)))
        throw Conflict("External HTTP request failed or exceeded its limits");
    return response;
}

std::string urlEncode(std::string_view value) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') encoded += static_cast<char>(c);
        else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
    }
    return encoded;
}

Json parse(std::string_view value) {
    constexpr int maxDepth = 24;
    const auto checkDepth = [](int depth,Json::parse_event_t,Json&) {
        if (depth > maxDepth) throw Conflict("External response is nested too deeply");
        return true;
    };
    auto result = Json::parse(value,checkDepth,false);
    if (!result.is_object()) throw Conflict("External service returned an invalid response");
    return result;
}

class PublicWeb final : public WebTools {
    const std::string token_;
public:
    explicit PublicWeb(std::string token) : token_(std::move(token)) {}
    Json search(std::string_view query) override {
        if (token_.empty()) throw Conflict("Web search is not configured");
        const auto url = "https://api.search.brave.com/res/v1/web/search?count=6&q=" + urlEncode(query);
        const auto response = http(url,true,{"Accept: application/json","X-Subscription-Token: " + token_});
        const auto json = SecretRedactor({token_}).payload(parse(response.body));
        const auto matches = json.value("web",Json::object()).value("results",Json::array());
        auto results = Json::array();
        if (!matches.is_array()) throw Conflict("Search returned an invalid results list");
        for (const auto& item : matches) {
            if (!item.is_object() || !item.contains("url") || !item["url"].is_string() ||
                item["url"].get_ref<const std::string&>().size() > 2048) continue;
            results.push_back({{"title",item.value("title","")},{"url",item["url"]},
                               {"description",item.value("description","")}});
            if (results.size() == 6) break;
        }
        return {{"results",results}};
    }
    Json fetch(std::string_view url) override {
        const auto result = http(std::string(url),true,{"Accept: text/html, text/plain, application/json"},nullptr,true);
        const auto type = result.type.substr(0,result.type.find(';'));
        if (!type.starts_with("text/") && type != "application/json" && type != "application/xhtml+xml")
            throw Conflict("Web page is not a supported text format");
        return {{"url",url},{"contentType",type},{"text",bounded(result.body,7000)},
                {"truncated",result.truncated || result.body.size() > 7000}};
    }
};

struct McpRejected : Conflict { using Conflict::Conflict; };

Json rpcResult(const Response& response, int id) {
    Json message;
    if (response.type.starts_with("text/event-stream")) {
        std::string event;
        std::string_view rest(response.body);
        while (!rest.empty()) {
            const auto end = rest.find('\n');
            auto line = rest.substr(0,end);
            rest.remove_prefix(end == rest.npos ? rest.size() : end+1);
            if (line.ends_with('\r')) line.remove_suffix(1);
            if (line.starts_with("data:")) {
                event += trim(line.substr(5));
                event += '\n';
            } else if (line.empty() && !event.empty()) {
                auto item = parse(event);
                if (item.contains("id") && item["id"] == id) { message = std::move(item); break; }
                event.clear();
            }
        }
    } else message = parse(response.body);
    if (!message.is_object() || message.value("jsonrpc","") != "2.0" ||
        !message.contains("id") || message["id"] != id)
        throw Conflict("MCP server returned an invalid response");
    if (!message.contains("result") && message.contains("error") && message.at("error").is_object()) {
        const auto& error = message.at("error");
        const auto code = error.value("code",Json());
        if (code.is_number_integer() && error.contains("message") && error.at("message").is_string() &&
            (code == -32700 || code == -32600 || code == -32601 || code == -32602))
            throw McpRejected("MCP server rejected the request (" + code.dump() + ")");
    }
    if (response.status >= 300 || !message.contains("result") || !message["result"].is_object() || message.contains("error"))
        throw Conflict("MCP server returned an invalid or failed result");
    return std::move(message["result"]);
}

bool stringMember(const Json& object, const char* key) {
    const auto field = object.find(key);
    return field != object.end() && field->is_string();
}

bool validMcpContentBlock(const Json& block) {
    const auto type = block.find("type");
    if (type == block.end() || !type->is_string()) return false;
    const auto& name = type->get_ref<const std::string&>();
    if (name == "text") return stringMember(block,"text");
    if (name == "image" || name == "audio") return stringMember(block,"data") && stringMember(block,"mimeType");
    if (name == "resource_link") return stringMember(block,"uri") && stringMember(block,"name");
    if (name == "resource") {
        const auto resource = block.find("resource");
        return resource != block.end() && stringMember(*resource,"uri") &&
               (stringMember(*resource,"text") || stringMember(*resource,"blob"));
    }
    // Forward unfamiliar block types as opaque data.
    return !name.empty();
}

void validateMcpToolContent(const Json& result) {
    const auto content = result.find("content");
    const auto structured = result.find("structuredContent");
    if (content == result.end() || !content->is_array() || !std::ranges::all_of(*content,validMcpContentBlock) ||
        (structured != result.end() && !structured->is_object()))
        throw Conflict("Invalid MCP tool content");
}

struct Server {
    std::string url, token;
};

struct McpSession {
    const Server& server;
    std::vector<std::string> headers{"Accept: application/json, text/event-stream","Content-Type: application/json"};
    bool tools = false, resources = false, stateful = false;
    int id = 0;

    Response post(const Json& request) const {
        return http(server.url,false,headers,request);
    }

    explicit McpSession(const Server& settings) : server(settings) {
        if (!server.token.empty()) headers.push_back("Authorization: Bearer " + server.token);
        auto init = post({{"jsonrpc","2.0"},{"id",++id},{"method","initialize"},
                          {"params",{{"protocolVersion",protocol},{"capabilities",Json::object()},
                                     {"clientInfo",{{"name","Flock"},{"version","0.1"}}}}}});
        const auto result = rpcResult(init,id);
        auto version = result.value("protocolVersion","");
        if (version != protocol && version != "2025-06-18" && version != "2025-11-25")
            throw Conflict("MCP server selected an unsupported protocol version");
        headers.push_back("MCP-Protocol-Version: " + version);
        if (!init.session.empty()) {
            if (!safeHeaderValue(init.session)) throw Conflict("MCP server returned an invalid session ID");
            headers.push_back("Mcp-Session-Id: " + init.session);
            stateful = true;
        }
        try {
            const auto caps = result.value("capabilities",Json::object());
            tools = caps.contains("tools");
            resources = caps.contains("resources");
            post({{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        } catch (...) {
            close();
            throw;
        }
    }

    ~McpSession() { close(); }

    void close() noexcept {
        if (!stateful) return;
        try { http(server.url,false,headers,nullptr,false,true); }
        catch (...) {}
    }

    Json callRaw(std::string_view method, Json params) {
        const auto response = post({{"jsonrpc","2.0"},{"id",++id},{"method",method},{"params",std::move(params)}});
        return rpcResult(response,id);
    }

    Json scrub(Json result) const {
        if (server.token.empty()) return result;
        return SecretRedactor({server.token}).payload(result);
    }

    Json call(std::string_view method, Json params) {
        return scrub(callRaw(method,std::move(params)));
    }
};

class ConfiguredMcp final : public McpTools {
    using Clock = std::chrono::steady_clock;
    struct Page {
        std::string bot, server, cursor;
        McpCollection collection;
        Clock::time_point expires;
        std::unique_ptr<McpSession> session;
    };
    const std::map<std::string,Server,std::less<>> servers_;
    std::mutex mutex_;
    std::map<std::string,Page,std::less<>> pages_;

    Page takePage(std::string_view bot, std::string_view server, McpCollection collection, std::string_view cursor) {
        std::vector<std::unique_ptr<McpSession>> expired;
        std::lock_guard lock(mutex_);
        const auto now = Clock::now();
        for (auto page = pages_.begin(); page != pages_.end();) {
            if (page->second.expires > now) ++page;
            else {
                expired.push_back(std::move(page->second.session));
                page = pages_.erase(page);
            }
        }
        if (cursor.empty()) return {std::string(bot),std::string(server),{},collection,{},{}};
        const auto found = pages_.find(cursor);
        if (found == pages_.end() || found->second.bot != bot || found->second.server != server ||
            found->second.collection != collection) throw Conflict("MCP discovery cursor is unavailable");
        return std::move(pages_.extract(found).mapped());
    }

public:
    explicit ConfiguredMcp(std::map<std::string,Server,std::less<>> servers) : servers_(std::move(servers)) {}
    bool hasServer(std::string_view server) const override { return servers_.contains(server); }
    Json discover(std::string_view bot, std::string_view server, McpCollection collection, std::string_view cursor) override {
        auto page = takePage(bot,server,collection,cursor);
        if (!page.session) page.session = std::make_unique<McpSession>(servers_.at(page.server));
        const bool tools = collection == McpCollection::tools;
        const auto key = tools ? "tools" : "resources";
        if (!(tools ? page.session->tools : page.session->resources)) return {{key,Json::array()}};
        auto params = Json::object();
        if (!page.cursor.empty()) params["cursor"] = page.cursor;
        auto result = page.session->callRaw(tools ? "tools/list" : "resources/list",std::move(params));
        page.cursor = result.value("nextCursor","");
        result.erase("nextCursor");
        result = page.session->scrub(std::move(result));
        if (page.cursor.empty()) return result;

        const auto next = actionId();
        page.expires = Clock::now() + std::chrono::minutes(2);
        {
            std::lock_guard lock(mutex_);
            if (pages_.size() >= 16) throw Conflict("Too many MCP discovery pages are open");
            pages_.emplace(next,std::move(page));
        }
        result["nextCursor"] = next;
        return result;
    }
    McpToolResult invoke(std::string_view server, std::string_view name, const Json& args) override {
        McpSession session(servers_.at(std::string(server)));
        if (!session.tools) throw Conflict("MCP server does not offer tools");
        try {
            auto result = session.callRaw("tools/call",{{"name",name},{"arguments",args}});
            validateMcpToolContent(result);
            const bool isError = result.value("isError",false);
            return {session.scrub(std::move(result)),isError};
        }
        catch (const McpRejected&) { throw; }
        catch (const std::exception&) { throw ToolOutcomeUnknown("The MCP tool outcome is unknown"); }
    }
    Json readResource(std::string_view server, std::string_view uri) override {
        McpSession session(servers_.at(std::string(server)));
        if (!session.resources) throw Conflict("MCP server does not offer resources");
        return session.call("resources/read",{{"uri",uri}});
    }
};

std::string mcpUrl(const std::string& url) {
    const std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> handle(curl_url(),curl_url_cleanup);
    if (!handle || url.size() > 2048 || url.find('\0') != url.npos || url.find_first_of("\r\n\t \\") != url.npos ||
        curl_url_set(handle.get(),CURLUPART_URL,url.c_str(),CURLU_DISALLOW_USER) != CURLUE_OK)
        throw Conflict("Invalid MCP endpoint");
    const auto get = [&](CURLUPart key) {
        char* raw = nullptr;
        if (curl_url_get(handle.get(),key,&raw,0) != CURLUE_OK) return std::string{};
        const std::unique_ptr<char,decltype(&curl_free)> part(raw,curl_free);
        return std::string(part.get());
    };
    const auto host = get(CURLUPART_HOST);
    const auto scheme = get(CURLUPART_SCHEME);
    if (host.empty() || (scheme != "https" && !(scheme == "http" && (host == "127.0.0.1" || host == "[::1]"))))
        throw Conflict("MCP endpoints require HTTPS or a numeric HTTP loopback address");
    return get(CURLUPART_URL);
}
}

std::shared_ptr<const BotTools> loadBotTools(const std::filesystem::path& home) {
    auto result = std::make_shared<BotTools>();
    result->web = std::make_shared<PublicWeb>(environmentToken("FLOCK_WEB_SEARCH_KEY"));
    const auto file = home / "bot-tools.json";
    if (!std::filesystem::exists(file)) return result;
    if (!std::filesystem::is_regular_file(file) || std::filesystem::file_size(file) > 65536)
        throw Conflict("Invalid Bot tools configuration file");
    std::ifstream input(file);
    const auto config = parse(std::string(std::istreambuf_iterator<char>(input),{}));
    const auto resolve = [&](const std::string& value) {
        if (value.empty() || value.find('\0') != value.npos) throw Conflict("Invalid Bot tools directory");
        return home / value;
    };
    for (const auto& [key,value] : config.items())
        if (key != "skills" && key != "mcpServers") throw Conflict("Unknown Bot tools setting " + key);
    const auto skills = config.value("skills",Json::object());
    if (!skills.is_object() || skills.size() > 128) throw Conflict("Invalid skills configuration");
    for (const auto& [id,value] : skills.items()) {
        if (!validBotId(id) || id == "catalog" || !value.is_string()) throw Conflict("Invalid configured skill");
        result->skills.emplace(id,std::filesystem::canonical(resolve(value.get<std::string>())));
    }
    const auto servers = config.value("mcpServers",Json::object());
    if (!servers.is_object() || servers.size() > 64) throw Conflict("Invalid MCP servers configuration");
    std::map<std::string,Server,std::less<>> configured;
    for (const auto& [id,value] : servers.items()) {
        if (!validBotId(id) || !value.is_object()) throw Conflict("Invalid configured MCP server");
        const auto tokenEnv = value.value("tokenEnv","");
        const auto token = tokenEnv.empty() ? "" : environmentToken(tokenEnv);
        if (!tokenEnv.empty() && token.empty()) throw Conflict("MCP server credential is missing");
        auto url = mcpUrl(value.at("url").get<std::string>());
        configured.emplace(id,Server{std::move(url),token});
    }
    result->mcp = std::make_shared<ConfiguredMcp>(std::move(configured));
    return result;
}
}
