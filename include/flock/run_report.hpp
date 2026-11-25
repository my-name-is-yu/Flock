#pragma once

#include "flock/run_ref.hpp"

namespace flockv0 {
struct RunEvidence {
    enum class Availability { Available, ReportUnavailable, DiagnosticUnavailable };
    Availability availability = Availability::Available;
    std::string report, itemId, detail;
    bool truncated = false;
    std::string sessionCreatedAt{};
    bool operator==(const RunEvidence&) const = default;
};

struct SettledRunReport {
    AssignedRun run;
    std::string resultRunId, status;
    RunEvidence evidence;
    bool stopRequested = false;
};
}
