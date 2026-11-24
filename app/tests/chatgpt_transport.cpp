#include "../chatgpt.hpp"

#include <iostream>

using namespace flockv0;
using namespace flockv0::app::chatgpt;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Feed chunks to the real SSE parser as curl would. Leave the connection
// open at completion and require the parser to stop reading it.
HttpTransport eventStream(std::initializer_list<std::string_view> chunks) {
    return [chunks = std::vector<std::string_view>(chunks)](const HttpRequest& request) {
        check(request.url == std::string(resource) + "/responses" &&
              request.accept == "text/event-stream" && request.contentType == "application/json" &&
              request.authorization == "Bearer fixture" && request.body == "{}",
              "unexpected Responses request");
        const HttpResponse headers{200,"Text/Event-Stream; charset=utf-8","fixture-request",{}};
        for (const auto chunk : chunks) {
            if (!request.onData(chunk,headers)) return headers;
        }
        return headers;
    };
}

void expectFailure(const HttpTransport& transport, bool transient, std::string_view message, Json usage = Json::object()) {
    try {
        infer(transport,"fixture","{}");
    } catch (const ModelError& error) {
        check(error.transient == transient && std::string_view(error.what()).find(message) != message.npos &&
              error.usage == usage, "unexpected ChatGPT failure");
        check(std::string_view(error.what()).find("private provider message") == message.npos,
              "raw remote error appeared in the model failure");
        return;
    }
    throw std::runtime_error("ChatGPT transport accepted a failed response");
}

void checkStream() {
    auto transport = eventStream({"\xef\xbb\xbf: heartbeat\r","\n\r\n",
        "data: {\"type\":\"response.completed\",\r\n",
        "data: \"response\":{\"output\":[{\"text\":\"\xe7", "\xb5\x82\"}],\"usage\":{\"input_tokens\":6}}}\r\n",
        "\r\n", "this invalid tail should never be read: \xff"});
    const auto result = infer(transport,"fixture","{}");
    check(result.at("output").at(0).at("text") == "終" &&
          tokenUsage(result.at("usage")) == Json{{"input_tokens",6}}, "SSE output or usage was lost");

    expectFailure(eventStream({"data: {invalid}\n\n"}),false,"Invalid JSON");
    expectFailure(eventStream({"\xff"}),false,"Invalid UTF-8");
    expectFailure(eventStream({"\xe3\x81"}),false,"Invalid UTF-8");
    expectFailure(eventStream({"data: {}\n"}),true,"ended before");
    expectFailure(eventStream({"data: {\"type\":\"response.incomplete\","
                               "\"response\":{\"usage\":{\"output_tokens\":2}}}\n\n"}),
                  false,"incomplete response",{{"output_tokens",2}});
    expectFailure(eventStream({"data: {\"type\":\"error\",\"code\":\"server_error\"}\n\n"}),
                  true,"request fixture-request");
    expectFailure([](const auto&) { return HttpResponse{200,"text/plain",{}, {}}; },
                  false,"expected response stream");
}

void checkProviderErrors() {
    const auto rejected = [](long status, std::string code) -> HttpTransport {
        return [status,code = std::move(code)](const auto&) {
            return HttpResponse{status,"application/json","fixture-request",Json{
                {"error",{{"code",code},{"message","private provider message"}}}}.dump()};
        };
    };
    expectFailure(rejected(400,"feature_unavailable"),false,"feature_unavailable");
    expectFailure(rejected(503,"test"),true,"HTTP 503");
    expectFailure(rejected(429,"subscription_sharing_usage_limit_exceeded"),false,"settings/usage");
    expectFailure([](const auto&) -> HttpResponse { throw ModelError("Unable to reach ChatGPT",true); },
                  true,"Unable to reach");
}

void checkCatalog() {
    const auto catalog = models([](const HttpRequest& request) {
        check(request.url == std::string(resource) + "/models" && request.authorization == "Bearer fixture" &&
              request.body.empty() && !request.onData, "unexpected model catalog request");
        return HttpResponse{200,"application/json",{},R"({"models":[
            {"slug":"a","visibility":"list","display_name":"A"},
            {"slug":"b","visibility":"list"},
            {"slug":"c","visibility":"hidden"},
            {"visibility":"list"}
        ]})"};
    },"fixture");
    check(catalog == Json::array({{{"slug","a"},{"displayName","A"}},{{"slug","b"},{"displayName","b"}}}),
          "model catalog filter changed");
}
}

int main() {
    try {
        checkStream();
        checkProviderErrors();
        checkCatalog();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
