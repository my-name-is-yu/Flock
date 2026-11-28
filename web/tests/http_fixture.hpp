#pragma once

#include "flock/web.hpp"
#include "../../tests/support.hpp"
#include "flock/decision.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/runtime.hpp"
#include "flock/task_interaction.hpp"

#include <nlohmann/json.hpp>

#include <httplib.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace flockv0::http_test {

inline constexpr auto privateValue = "private-token-value";

using flockv0::test::check;
using flockv0::test::TemporaryDirectory;

struct FixedModel : Model {
    ModelTurn decide(const ModelInput& input) override {
        const auto& event = input.event;
        if (event.contains("sessionWake") && event.contains("task") && event.at("turn").empty())
            return {{{"complete", {{"report", "Draft complete"}}, "The Session report matches the request"}}};
        return {{{"reply", {{"text", "Done."}}, {}}}};
    }
};

inline Question scopeQuestion() {
    return {"scope-question",
            {{.id = "scope",
              .prompt = "利用者向けの変更だけを載せますか？",
              .options = {{"利用者向けのみ", "利用者向けのみ"}, {"すべて", "すべて"}}}}};
}

inline WebCallbacks callbacks() {
    return {[] {
                return Json::array({
                    {{"id", "release"},
                     {"name", "Release"},
                     {"soul", "利用者向けのリリースノートをまとめる。"},
                     {"cron", Json::array({{{"id", "daily"},
                                            {"schedule", "0 9 * * *"},
                                            {"timezone", "Asia/Tokyo"},
                                            {"request", "前日の変更を確認する"},
                                            {"token", privateValue}}})},
                     {"avatar", {{"example", "mug"}, {"color", "#982fff"}, {"secret", privateValue}}},
                     {"token", privateValue}},
                    {{"id", "triage"}, {"name", "Triage"}, {"soul", "報告された問題を整理する。"}, {"cron", Json::array()}},
                    {{"id", "settings"}, {"name", "Settings"}, {"soul", "設定エラーの確認"}},
                    {{"id", "broken"}, {"name", "Broken"}},
                });
            },
            [](std::string_view bot, std::string_view request) -> Intake {
                if (bot == "broken") throw std::runtime_error(privateValue);
                return {std::string(bot),
                        std::string(request),
                        bot == "triage" ? "linked env" : "local-env",
                        std::string(bot),
                        {{"token", privateValue}}};
            }};
}

class Browser {
public:
    explicit Browser(const WebServer& server)
        : client("127.0.0.1", static_cast<int>(server.port())), origin("http://127.0.0.1:" + std::to_string(server.port())) {}

    Json get(const std::string& path) { return checked(client.Get(path), 200); }
    Json post(const std::string& path, const Json& data, int status = 200) {
        try {
            return checked(client.Post(path, headers(), data.dump(), "application/json"), status);
        } catch (const std::exception& error) {
            throw std::runtime_error(path + ": " + error.what());
        }
    }
    httplib::Headers headers() const { return {{"Origin", origin}, {"Sec-Fetch-Site", "same-origin"}, {"X-Flock-Action", "1"}}; }
    static Json checked(const httplib::Result& response, int status) {
        if (!response) throw std::runtime_error("HTTP request failed: " + httplib::to_string(response.error()));
        if (response->status != status)
            throw std::runtime_error("Unexpected HTTP " + std::to_string(response->status) + " from " + response->body.substr(0, 240));
        check(response->body.find(privateValue) == std::string::npos, "private connection value entered HTTP response");
        return Json::parse(response->body);
    }
    httplib::Client client;
    std::string origin;
};

inline void tick(Runtime& runtime, int count) {
    for (int i = 0; i < count; ++i) runtime.tick();
}

void boardInboxEndpoints(const std::filesystem::path&);
void channelEndpoints(const std::filesystem::path&);
}
