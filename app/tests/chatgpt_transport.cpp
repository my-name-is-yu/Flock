#include "../chatgpt.hpp"
#include "../model.hpp"
#include "fixtures.hpp"
#include "flock/base64.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <future>
#include <iostream>

using namespace flockv0;
using namespace flockv0::app::chatgpt;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checkTlsDisconnect() {
    struct Socket {
        int fd;
        ~Socket() {
            if (fd >= 0) close(fd);
        }
    };
    const Socket listener{socket(AF_INET, SOCK_STREAM, 0)};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto size = static_cast<socklen_t>(sizeof(address));
    check(listener.fd >= 0 && bind(listener.fd, reinterpret_cast<const sockaddr*>(&address), size) == 0 && listen(listener.fd, 1) == 0 &&
              getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &size) == 0,
          "could not listen for the TLS fixture");
    check(setenv("no_proxy", "127.0.0.1", 1) == 0 && signal(SIGPIPE, SIG_DFL) != SIG_ERR,
          "could not set up the transport test environment");
    sigset_t sigpipe;
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);
    check(pthread_sigmask(SIG_UNBLOCK, &sigpipe, nullptr) == 0, "could not enable SIGPIPE for the test");
    [[maybe_unused]] const auto writer = pthread_self();
    auto peer = std::async(std::launch::async, [&] {
        pollfd incoming{listener.fd, POLLIN, 0};
        check(poll(&incoming, 1, 3000) == 1, "the native TLS client did not connect");
        const Socket client{accept(listener.fd, nullptr, nullptr)};
        pollfd handshake{client.fd, POLLIN, 0};
        check(client.fd >= 0 && poll(&handshake, 1, 3000) == 1, "the native TLS client did not start its handshake");
        unsigned char record = 0;
        check(recv(client.fd, &record, 1, 0) == 1 && record == 0x16, "expected a TLS handshake record");
#ifndef SO_NOSIGPIPE
        // Reproduce the signal delivered to a TLS writer on platforms without socket-level suppression.
        check(pthread_kill(writer, SIGPIPE) == 0, "could not signal the TLS writer");
#endif
    });
    HttpRequest request;
    request.url = "https://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    request.timeout = std::chrono::seconds(3);
    bool retryable = false;
    try {
        httpRequest(request);
    } catch (const ModelError& error) {
        retryable = error.transient;
    }
    peer.get();
    sigset_t current;
    check(retryable && pthread_sigmask(SIG_SETMASK, nullptr, &current) == 0 && sigismember(&current, SIGPIPE) == 0,
          "the TLS disconnect did not return a retryable failure and restore the signal mask");
}

// Feed chunks to the real SSE parser as curl would. Leave the connection
// open at completion and require the parser to stop reading it.
HttpTransport eventStream(std::initializer_list<std::string_view> chunks) {
    return [chunks = std::vector<std::string_view>(chunks)](const HttpRequest& request) {
        check(request.url == std::string(resource) + "/responses" && request.accept == "text/event-stream" &&
                  request.contentType == "application/json" && request.authorization == "Bearer fixture" && request.body == "{}",
              "unexpected Responses request");
        const HttpResponse headers{200, "Text/Event-Stream; charset=utf-8", "fixture-request", {}};
        for (const auto chunk : chunks) {
            if (!request.onData(chunk, headers)) return headers;
        }
        return headers;
    };
}

void expectFailure(const HttpTransport& transport, bool transient, std::string_view message, Json usage = Json::object(),
                   ModelErrorCode code = ModelErrorCode::inference_failed) {
    try {
        infer(transport, "fixture", "{}");
    } catch (const ModelError& error) {
        check(error.code == code && error.transient == transient && std::string_view(error.what()).find(message) != message.npos &&
                  error.usage == usage,
              "unexpected ChatGPT failure");
        check(std::string_view(error.what()).find("private provider message") == message.npos,
              "raw remote error appeared in the model failure");
        return;
    }
    throw std::runtime_error("ChatGPT transport accepted a failed response");
}

void checkStream() {
    auto transport = eventStream({"\xef\xbb\xbf: heartbeat\r", "\n\r\n", "data: {\"type\":\"response.completed\",\r\n",
                                  "data: \"response\":{\"output\":[{\"text\":\"\xe7",
                                  "\xb5\x82\"}],\"usage\":{\"input_tokens\":6,"
                                  "\"input_tokens_details\":{\"cached_tokens\":4,\"other\":3}}}}\r\n",
                                  "\r\n", "this invalid tail should never be read: \xff"});
    const auto result = infer(transport, "fixture", "{}");
    check(result.at("output").at(0).at("text") == "終" &&
              tokenUsage(result.at("usage")) == Json{{"input_tokens", 6}, {"input_tokens_details", {{"cached_tokens", 4}}}},
          "SSE output or cached usage was lost");

    expectFailure(eventStream({"data: {invalid}\n\n"}), false, "Invalid JSON");
    expectFailure(eventStream({"\xff"}), false, "Invalid UTF-8");
    expectFailure(eventStream({"\xe3\x81"}), false, "Invalid UTF-8");
    expectFailure(eventStream({"data: {}\n"}), true, "ended before");
    expectFailure(eventStream({"data: {\"type\":\"response.incomplete\","
                               "\"response\":{\"usage\":{\"output_tokens\":2,\"input_tokens_details\":{\"cached_tokens\":0}}}}\n\n"}),
                  false, "incomplete response", {{"output_tokens", 2}, {"input_tokens_details", {{"cached_tokens", 0}}}});
    expectFailure(eventStream({"data: {\"type\":\"response.completed\","
                               "\"response\":{\"usage\":{\"input_tokens_details\":{\"cached_tokens\":4}}}}\n\n"}),
                  false, "Invalid ChatGPT completion", {{"input_tokens_details", {{"cached_tokens", 4}}}});
    check(tokenUsage({{"input_tokens_details", {{"cached_tokens", -1}}}}).empty(), "negative cached usage was accepted");
    expectFailure(eventStream({"data: {\"type\":\"error\",\"code\":\"server_error\"}\n\n"}), true, "request fixture-request");
    expectFailure([](const auto&) { return HttpResponse{200, "text/plain", {}, {}}; }, false, "expected response stream");
}

void checkProviderErrors() {
    const auto rejected = [](long status, std::string code) -> HttpTransport {
        return [status, code = std::move(code)](const auto&) {
            return HttpResponse{status, "application/json", "fixture-request",
                                Json{{"error", {{"code", code}, {"message", "private provider message"}}}}.dump()};
        };
    };
    expectFailure(rejected(400, "feature_unavailable"), false, "feature_unavailable");
    expectFailure(rejected(400, "invalid_image"), false, "invalid_image", Json::object(), ModelErrorCode::image_rejected);
    expectFailure(eventStream({"data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"image_parse_error\"},"
                               "\"usage\":{\"input_tokens\":3}}}\n\n"}),
                  false, "image_parse_error", {{"input_tokens", 3}}, ModelErrorCode::image_rejected);
    expectFailure(rejected(503, "test"), true, "HTTP 503");
    expectFailure(rejected(429, "subscription_sharing_usage_limit_exceeded"), false, "settings/usage");
    expectFailure([](const auto&) -> HttpResponse { throw ModelError("Unable to reach ChatGPT", true); }, true, "Unable to reach");
}

void checkCatalog() {
    const auto catalog = models(
        [](const HttpRequest& request) {
            check(request.url == std::string(resource) + "/models" && request.authorization == "Bearer fixture" && request.body.empty() &&
                      !request.onData,
                  "unexpected model catalog request");
            return HttpResponse{200, "application/json", {}, R"({"models":[
            {"slug":"a","visibility":"list","display_name":"A","supported_reasoning_levels":[
                {"effort":"low","description":"More responsive"},{"effort":"ultra"},{"effort":"low"},{"effort":4}]},
            {"slug":"b","visibility":"list"},
            {"slug":"c","visibility":"hidden"},
            {"slug":"d","visibility":"list","supported_reasoning_levels":[]},
            {"visibility":"list"}
        ]})"};
        },
        "fixture");
    check(catalog == Json::array({
                         {{"slug", "a"}, {"displayName", "A"}, {"reasoningEfforts", {"low", "ultra"}}},
                         {{"slug", "b"}, {"displayName", "b"}},
                         {{"slug", "d"}, {"displayName", "d"}, {"reasoningEfforts", Json::array()}},
                     }),
          "model catalog lost the distinction between advertised efforts and missing metadata");
}

void checkUnsupportedReasoningEffort() {
    try {
        infer(
            [](const HttpRequest&) {
                return HttpResponse{400, "application/json", {}, R"({"error":{"code":"unsupported_value",
                "param":"reasoning.effort","message":"private provider detail"}})"};
            },
            "fixture", R"({"reasoning":{"effort":"low"}})");
        throw std::runtime_error("Unsupported effort was accepted");
    } catch (const ModelError& error) {
        const std::string_view message = error.what();
        check(error.code == ModelErrorCode::unsupported_reasoning_effort && !error.transient &&
                  message.find("Model default") != message.npos && message.find("private provider detail") == message.npos,
              "Unsupported effort did not give a safe, actionable error");
    }
}

std::vector<Json> readRecordings(const std::filesystem::path& path) {
    std::ifstream file(path);
    check(file.is_open(), "could not open ChatGPT recordings");
    std::vector<Json> records;
    for (std::string line; std::getline(file, line);) records.push_back(Json::parse(line));
    check(file.eof() && !file.bad(), "could not read ChatGPT recordings");
    return records;
}

HttpTransport replayResponse(const Json& exchange, const Json& expectedRequest) {
    return [&exchange, &expectedRequest](const HttpRequest& request) {
        check(request.url == std::string(resource) + "/responses" && request.accept == "text/event-stream" && request.onData &&
                  Json::parse(request.body) == expectedRequest,
              "recorded response did not match the Responses request");
        HttpResponse response{exchange.at("status").get<long>(), exchange.at("contentType").get<std::string>(), "fixture-request", {}};
        if (exchange.contains("body")) {
            response.body = exchange.at("body").get<std::string>();
            return response;
        }
        for (const auto& event : exchange.at("events")) {
            const auto frame = "data: " + event.dump() + "\r\n\r\n";
            for (std::size_t offset = 0; offset < frame.size(); offset += 13)
                if (!request.onData(std::string_view(frame).substr(offset, 13), response)) return response;
        }
        return response;
    };
}

Json batchRequest(const Json& recording) {
    ModelInput input{};
    input.config = {{"modelProvider", "chatgpt"}, {"model", recording.at("modelSlug")}};
    input.event = {{"kind", "conversation_turn"}};
    input.tools = {"flock_session_search", "check_environment_readiness"};
    return app::responseRequest(input);
}

void checkRecordedBatch(const Json& exchange, const Json& request, const Json* captureRequest = nullptr) {
    const auto response = infer(replayResponse(exchange, request), "fixture-auth", request.dump(), captureRequest);
    const auto calls = app::responseDecision(response.at("output"), request);
    check(calls.calls.size() == 2 && calls.rejections.empty() && calls.calls[0].tool == "flock_session_search" &&
              calls.calls[0].args.at("limit") == 2 && calls.calls[1].tool == "check_environment_readiness",
          "recording changed the order or validation of the two model calls");
    check(tokenUsage(response.at("usage")) ==
              Json{{"input_tokens", 6}, {"output_tokens", 2}, {"total_tokens", 8}, {"input_tokens_details", {{"cached_tokens", 4}}}},
          "recording lost ChatGPT usage fields");
}

void checkRecordedError(const Json& exchange, const Json& request, const Json* captureRequest = nullptr) {
    try {
        infer(replayResponse(exchange, request), "fixture-auth", request.dump(), captureRequest);
    } catch (const Failure& error) {
        check(error.code == ModelErrorCode::image_rejected && error.providerCode == "invalid_image" && !error.transient,
              "recording changed the HTTP error's classification");
        return;
    }
    throw std::runtime_error("recorded HTTP error was accepted");
}

void checkRecordings(const std::filesystem::path& recordingFile) {
    const auto records = readRecordings(recordingFile);
    check(records.size() == 2, "expected a streamed response and an HTTP error recording");
    for (const auto& record : records)
        check(record.at("schema") == 1 && (record.at("source") == "fixture" || record.at("source") == "real") &&
                  record.at("captureCommit").get_ref<const std::string&>().size() == 40,
              "invalid ChatGPT recording metadata");
    const auto batch = batchRequest(records[0]);
    const auto errorRequest = batchRequest(records[1]);
    checkRecordedBatch(records[0].at("exchange"), batch);
    checkRecordedError(records[1].at("exchange"), errorRequest);

    const app::test::Sandbox sandbox;
    const auto capturedFile = sandbox.path / "capture.jsonl";
    check(setenv("FLOCK_RECORD_CHATGPT", capturedFile.c_str(), 1) == 0 &&
              setenv("FLOCK_RECORD_CHATGPT_MODEL", records[0].at("modelSlug").get_ref<const std::string&>().c_str(), 1) == 0 &&
              setenv("FLOCK_RECORD_COMMIT", records[0].at("captureCommit").get_ref<const std::string&>().c_str(), 1) == 0,
          "could not enable ChatGPT capture");
    constexpr auto privateText = "private-person@example.test /Users/private-person/work";
    auto privateRequest = batch;
    privateRequest["input"][0]["content"][0]["text"] = privateText;
    auto stream = records[0].at("exchange");
    stream["events"][0]["response"]["id"] = "private-response";
    const auto firstEvent =
        std::ranges::find_if(stream["events"], [](const Json& event) { return event.value("type", "") == "response.output_item.added"; });
    check(firstEvent != stream["events"].end(), "recording needs an item lifecycle event");
    const auto firstId = firstEvent->at("item").at("id");
    const auto firstCallId = firstEvent->at("item").at("call_id");
    for (auto& event : stream["events"]) {
        if (event.contains("item_id") && event.at("item_id") == firstId) event["item_id"] = "private-item";
        if (event.contains("item") && event.at("item").at("id") == firstId) {
            event["item"]["id"] = "private-item";
            event["item"]["call_id"] = "private-call";
        }
    }
    auto& finalResponse = stream["events"].back()["response"];
    finalResponse["id"] = "private-response";
    finalResponse["metadata"] = {{"authorization", "Bearer fixture-auth"}, {"path", privateText}};
    auto& firstCall = finalResponse["output"][0];
    firstCall["id"] = "private-item";
    check(firstCall.at("call_id") == firstCallId, "fixture changed call identifiers across event types");
    firstCall["call_id"] = "private-call";
    firstCall["arguments"] = Json{{"query", privateText}, {"cursor", "private-cursor"}, {"limit", 2}, {"fromTime", nullptr}}.dump();
    checkRecordedBatch(stream, privateRequest, &privateRequest);
    auto error = records[1].at("exchange");
    error["body"] = Json{{"error", {{"code", "invalid_image"}, {"message", privateText}}}}.dump();
    checkRecordedError(error, errorRequest, &errorRequest);
    const Json failure{{"type", "response.failed"}, {"response", {{"error", {{"code", "invalid_image"}, {"message", privateText}}}}}};
    checkRecordedError({{"status", 200}, {"contentType", "text/event-stream"}, {"events", Json::array({failure})}}, batch, &batch);

    const auto captured = readRecordings(capturedFile);
    check(captured.size() == 3 && std::ranges::all_of(captured, [](const auto& record) { return record.at("source") == "real"; }),
          "capture did not produce both stream outcomes and the HTTP error");
    const auto capturedText = Json(captured).dump();
    const std::string_view privatePieces[]{privateText,      "private-person", "private-response", "private-item", "private-call",
                                           "private-cursor", "fixture-auth",   "fixture-request",  "authorization"};
    for (const auto secret : privatePieces) check(capturedText.find(secret) == capturedText.npos, "capture retained private data");
    const auto& events = captured[0].at("exchange").at("events");
    const auto& completed = events.back().at("response");
    const auto& call = completed.at("output").front();
    const auto args = Json::parse(call.at("arguments").get_ref<const std::string&>());
    const auto& firstItem = events.at(static_cast<std::size_t>(firstEvent - stream.at("events").begin())).at("item");
    check(events.size() == stream.at("events").size() && completed.at("id") == events.front().at("response").at("id") &&
              call.at("id") == firstItem.at("id") && call.at("call_id") == firstItem.at("call_id") && args.at("fromTime").is_null() &&
              !args.contains("toTime") && args.at("cursor").is_string(),
          "capture lost event order, ID correspondence, or null versus missing");
    checkRecordedBatch(captured[0].at("exchange"), batch);
    checkRecordedError(captured[1].at("exchange"), errorRequest);
    checkRecordedError(captured[2].at("exchange"), batch);
}

void checkInspectedImageRequest() {
    const app::test::Sandbox credentials;
    std::filesystem::permissions(credentials.path, std::filesystem::perms::owner_all);
    const auto profile = credentials.path / "profile.json";
    const Json token{{"subject", "fixture"},
                     {"accessToken", "fixture"},
                     {"refreshToken", "fixture-refresh"},
                     {"granted", Json::array({directScope})},
                     {"expiresAt", nowMillis() + 3'600'000}};
    app::test::write(profile, token.dump());
    std::filesystem::permissions(profile, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    ModelInput input{};
    input.config = {{"modelProvider", "chatgpt"}, {"model", "fixture"}};
    input.event = {{"kind", "conversation_turn"}};
    input.tools = {"reply"};
    for (std::size_t index = 0; index < 4; ++index) {
        const auto bytes = std::string(2 * 1024 * 1024, static_cast<char>('a' + index));
        input.images.push_back({1, std::to_string(index + 1), "image/png", encodeBase64(bytes)});
    }

    unsigned requests = 0;
    app::ChatGPTModel model(credentials.path, [&](const HttpRequest& request) {
        ++requests;
        check(request.url == std::string(resource) + "/responses" && request.authorization == "Bearer fixture",
              "inspected images did not reach the authenticated Responses endpoint");
        const auto body = Json::parse(request.body);
        const auto& content = body.at("input").at(0).at("content");
        std::size_t imageIndex = 0;
        constexpr std::string_view prefix = "data:image/png;base64,";
        for (const auto& part : content) {
            if (part.at("type") != "input_image") continue;
            const std::string_view url = part.at("image_url").get_ref<const std::string&>();
            check(imageIndex < input.images.size() && url.starts_with(prefix) &&
                      url.substr(prefix.size()) == input.images[imageIndex].dataBase64,
                  "an inspected image was duplicated, reordered, or altered");
            ++imageIndex;
        }
        check(imageIndex == input.images.size(), "the Responses request lost an inspected image");
        const Json reply{{"type", "function_call"},
                         {"name", "reply"},
                         {"namespace", "flock"},
                         {"status", "completed"},
                         {"arguments", Json{{"text", "Reviewed the images"}}.dump()}};
        const Json event{{"type", "response.completed"}, {"response", {{"output", Json::array({reply})}}}};
        const HttpResponse response{200, "text/event-stream", "fixture-request", {}};
        request.onData("data: " + event.dump() + "\n\n", response);
        return response;
    });
    const auto turn = model.decide(input);
    check(requests == 1 && turn.calls.size() == 1 && turn.calls.front().tool == "reply",
          "four admitted images did not complete in one Responses request");
}

void checkTlsStream(const char* url, const char* caFile) {
    bool completed = false;
    const auto result = infer(
        [&](const HttpRequest& request) {
            auto local = request;
            local.url = url;
            local.caFile = caFile;
            local.onData = [&](std::string_view chunk, const HttpResponse& headers) {
                check(headers.status == 200 && headers.contentType == "Text/Event-Stream; charset=utf-8" &&
                          headers.requestId == "fixture-request",
                      "libcurl did not supply response metadata before streaming");
                completed = !request.onData(chunk, headers);
                return !completed;
            };
            return httpRequest(local);
        },
        "fixture", "{}");
    check(completed && result.at("output").at(0).at("text") == "終" &&
              tokenUsage(result.at("usage")) == Json{{"input_tokens", 6}, {"output_tokens", 2}, {"total_tokens", 8}},
          "native HTTPS did not return the completed SSE output and usage");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--tls") {
            checkTlsStream(argv[2], argv[3]);
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--recordings") {
            checkRecordings(argv[2]);
            return 0;
        }
        check(argc == 1, "expected --tls URL CA-file, --recordings FILE, or no arguments");
        checkTlsDisconnect();
        checkStream();
        checkProviderErrors();
        checkCatalog();
        checkInspectedImageRequest();
        checkUnsupportedReasoningEffort();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
