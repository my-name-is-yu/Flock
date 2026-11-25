#include "internal.hpp"

#include <iostream>

using namespace flockv0;
using namespace flockv0::t3detail;

namespace {

void probe(const std::string& origin, const std::string& project, const std::string& action, const std::string& missingThread) {
    Client client({.mcpUrl = origin + "/mcp", .token = "fixture.signed-token", .generation = {}, .label = {}});
    const auto launch =
        client.call("t3_thread_launch", {{"projectId", project},
                                         {"clientRequestId", action},
                                         {"title", "Flock task"},
                                         {"message", "Check /Users/fixture/private@example.invalid\n\n[Flock action ID: " + action + "]"}});
    const auto thread = launch.at("threadId").get<std::string>();
    const auto run = launch.at("runId").get<std::string>();
    client.call("t3_thread_wait", {{"threadId", thread}, {"runId", run}, {"timeoutMs", 400}, {"untilDelegatedWorkSettled", true}});

    Id position = -1;
    for (const bool hasMore : {true, false}) {
        const auto start = position;
        auto page = readThread(client, thread, "messages", 1, 100, {{"afterPosition", start}});
        for (const auto& item : page.at("items")) advancePosition(item, position);
        if (moreHistory(page, position, start) != hasMore) throw std::runtime_error("T3 recording did not advance through two pages");
    }

    try {
        readThread(client, missingThread, "messages", 1, 100, {{"afterPosition", -1}});
    } catch (const Failure& error) {
        if (error.kind == Failure::Kind::Tool && error.reason == Failure::Reason::ThreadNotFound) return;
        throw;
    }
    throw std::runtime_error("T3 recording did not return the expected tool error");
}

}

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    try {
        probe(argv[1], argv[2], argv[3], argv[4]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
