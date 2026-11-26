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
struct BotTools;

struct RuntimeOptions {
    std::function<Seconds()> clock = unixTime;
    // "off", "on_demand", "prefill". Prefill uses the same bounded T3 history reader.
    std::string sessionRead = "off";
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
}
