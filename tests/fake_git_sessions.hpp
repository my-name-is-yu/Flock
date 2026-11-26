#pragma once

#include "flock/command.hpp"
#include "flock/t3.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace git_fixture {
using namespace flockv0;

class Sessions final : public T3 {
public:
    std::string authGeneration(const std::string&, const Json&) override { return "t3-fixture"; }
    std::vector<CommandPayload> sessionSends;
    std::optional<SessionTarget> askOrigin;
    std::function<void()> afterSessionSend;

    std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool, SessionReadDeadline) override;
    Delivery send(const Command&) override;
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef&) override {
        Observation result;
        result.kind = Observation::Kind::Finished;
        result.status = "completed";
        result.report = "Pushed branch change with tests";
        return result;
    }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { return InterruptOutcome::Requested; }
};
}
