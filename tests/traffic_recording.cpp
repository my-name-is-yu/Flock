#include "flock/detail/traffic_recording.hpp"
#include "support.hpp"
#include "flock/json.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

using namespace flockv0;
using namespace flockv0::recording;

namespace {

using flockv0::test::check;

void checkIds() {
    Ids ids;
    check(ids.replace("thread", "opaque-a") == "thread-1" && ids.replace("thread", "opaque-b") == "thread-2" &&
              ids.replace("thread", "opaque-a") == "thread-1" && ids.replace("run", "opaque-a") == "run-1",
          "opaque IDs lost their correspondence or domain");
    check(ids.replace("item", nullptr).is_null() && ids.replace("cursor", "") == "" && ids.replace("run", false) == false &&
              ids.replace("cursor", 193).is_number_integer() && ids.replace("cursor", 193.0).is_number_float() &&
              ids.replace("cursor", 193) != 193 && ids.replace("cursor", Json{{"private", "data"}}) == Json::object(),
          "placeholder changed the wire type or kept private data");
}

void checkSink(const std::filesystem::path& path) {
    const std::string commit(40, 'a');
    Capture capture(path, Provider::ChatGPT, commit, "gpt-5-test");
    const auto accept = [](Ids& ids) {
        return Json{{"output", Json::array({ids.replace("item", "opaque-a"), ids.replace("item", "opaque-b")})}};
    };
    const Json unsafe[] = {
        {{"error", "Bearer fixture-secret"}},
        {{"code", "sk-" + std::string(40, 'a')}},
        {{"details", {{"refreshToken", "synthetic"}}}},
        {{"arguments", R"({"access_token":"synthetic"})"}},
        {{"error", "user@example.org"}},
        {{"error", "/Users/fixture/private"}},
    };
    for (const auto& value : unsafe) {
        check(!capture.append("gpt-5-test", [&](Ids&) { return value; }) && !std::filesystem::exists(path),
              "capture created a file for potential private data");
    }
    check(capture.append("account-name", accept) && !std::filesystem::exists(path), "capture persisted an unselected model slug");
    check(capture.append("gpt-5-test", [](Ids&) { return nullptr; }) && !std::filesystem::exists(path),
          "capture created a file for an excluded exchange");
    check(capture.append("gpt-5-test", accept), "capture refused a safe exchange");
    check(!capture.append("gpt-5-test", [&](Ids&) { return unsafe[0]; }), "capture accepted a secret after opening the file");
    check(capture.append("gpt-5-test", accept), "capture could not continue after a refusal");
    Capture collision(path, Provider::ChatGPT, commit);
    check(!collision.append("gpt-5-test", accept), "capture overwrote an existing file");

    using std::filesystem::perms;
    check((std::filesystem::status(path).permissions() & (perms::group_all | perms::others_all)) == perms::none,
          "capture file was accessible to other users");
    const Json expected = {{"schema", 1},
                           {"source", "real"},
                           {"captureCommit", commit},
                           {"modelSlug", "gpt-5-test"},
                           {"exchange", {{"output", {"item-1", "item-2"}}}}};
    std::ifstream file(path);
    std::string line;
    check(std::getline(file, line) && Json::parse(line) == expected, "capture wrote the wrong first exchange");
    check(std::getline(file, line) && Json::parse(line) == expected && !std::getline(file, line),
          "capture wrote private data or lost ID correspondence");
}

}

int main() {
    const flockv0::test::TemporaryDirectory scratch;
    int result = 0;
    try {
        checkIds();
        checkSink(scratch.path / "traffic.jsonl");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    return result;
}
