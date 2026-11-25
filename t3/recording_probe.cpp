#include "internal.hpp"

#include <iostream>

using namespace flockv0;
using namespace flockv0::t3detail;

namespace {

Json readPage(Client& client, const std::string& thread, Json args, Id after) {
    const auto view = args.at("view").get<std::string>();
    const auto limit = args.at("limit").get<unsigned>();
    const auto chars = args.at("maxCharsPerItem").get<int>();
    const auto runs = args.at("runLimit").get<int>();
    args["afterPosition"] = after;
    return readThread(client, thread, view, limit, chars, std::move(args), runs);
}

void probe(const std::string& origin, const Json& plan) {
    Client client({.mcpUrl = origin + "/mcp", .token = "fixture.signed-token", .generation = {}, .label = {}});
    auto launchArgs = plan.at(0);
    launchArgs["title"] = "Flock task";
    launchArgs["message"] =
        "Check /Users/fixture/private@example.invalid\n\n[Flock action ID: " + launchArgs.at("clientRequestId").get<std::string>() + "]";
    const auto launch = client.call("t3_thread_launch", std::move(launchArgs));
    const auto thread = launch.at("threadId").get<std::string>();
    const auto run = launch.at("runId").get<std::string>();
    auto waitArgs = plan.at(1);
    waitArgs.update(Json{{"threadId", thread}, {"runId", run}});
    client.call("t3_thread_wait", std::move(waitArgs));

    Id position = -1;
    for (const auto index : {2, 3}) {
        const auto start = position;
        const auto page = readPage(client, thread, plan.at(index), start);
        for (const auto& item : page.at("items")) advancePosition(item, position);
        if (moreHistory(page, position, start) != (index == 2)) throw std::runtime_error("T3 recording did not advance through two pages");
    }

    try {
        const auto& missing = plan.at(4);
        readPage(client, missing.at("threadId").get<std::string>(), missing, -1);
    } catch (const Failure& error) {
        if (error.kind == Failure::Kind::Tool && error.reason == Failure::Reason::ThreadNotFound) return;
        throw;
    }
    throw std::runtime_error("T3 recording did not return the expected tool error");
}

}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        probe(argv[1], Json::parse(argv[2]));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
