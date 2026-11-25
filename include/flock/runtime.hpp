#pragma once

#include "flock/bot_change.hpp"
#include "flock/intake.hpp"
#include "flock/types.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace flockv0 {
class GitHost;
class Model;
class Store;
class T3;
struct BotTools;
struct RuntimeOptions {
    std::function<Seconds()> clock = unixTime;
    // "off", "on_demand", "prefill". Prefill uses the same bounded T3 history reader.
    std::string sessionRead = "off";
    // Resolve the peers this sender can use, with each peer's own execution settings.
    PeerResolver peers = {};
    std::function<void(std::string_view)> automationError = {};
    std::shared_ptr<const BotTools> botTools = {};
    std::shared_ptr<GitHost> gitHost = {};
    std::function<void(std::string_view)> storageContention = {};
    ApplyBotChange applyBotChange = {};
    // Supply this when applyBotChange can throw after publishing. On exhaustion,
    // confirm publication by request key or release any unpublished reservation.
    ReconcileBotCreation reconcileBotCreation = {};
    IntakeResolver intake = {};
    bool requireT3Connection = false;
};
class Runtime {
public:
    using SoulLoader = std::function<std::string(std::string_view)>;
    Runtime(Store&, Model&, T3&, SoulLoader, RuntimeOptions = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    void runConversations();
    // tick() completes a step for each eligible project. tickConcurrent() schedules
    // eligible projects independently and returns immediately.
    void tick();
    void tickConcurrent();
    // Let admitted work settle while stopping further dispatch.
    void requestStop();
private:
    struct Components;
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
