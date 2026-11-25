#pragma once

#include "flock/json.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "flock/session_ref.hpp"
#include "flock/t3_command.hpp"
#include "flock/t3_session.hpp"
#include "flock/task_interaction.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0 {
struct Observation {
    enum class Kind { Running, Question, ExternalInput, Settling, Finished, Unavailable } kind = Kind::Running;
    Question question;
    std::string status, report, resultRunId, itemId;
    bool truncated = false;
    std::string detail;
    RunEvidence::Availability evidence = RunEvidence::Availability::Available;
    std::string sessionCreatedAt{};
    std::string checkedCommit{};
    bool commitEvidenceUnavailable = false;
};
enum class InterruptOutcome { Requested, AlreadyTerminal, NoActiveRun, TargetGone };
enum class QueueCancelOutcome { Cancelled, AlreadyStarted, NotCancellable, Retry };

struct T3Project {
    std::string id, title, directory;
};
struct T3EnvironmentRoute {
    std::string environmentId, gatewayEnvironmentId;
};
struct T3EnvironmentLink {
    enum class Status { reachable, unreachable, expired };
    T3EnvironmentRoute route;
    std::string label;
    Status status = Status::unreachable;
};
struct T3EnvironmentLinks {
    std::vector<T3EnvironmentLink> links;
    std::string unavailable;
};

class T3 {
public:
    virtual ~T3() = default;
    virtual std::string sessionMessageWake(const SessionTarget&, const RunRef&) { return "unknown"; }
    virtual QueueCancelOutcome cancelQueuedSessionMessage(const SessionTarget&, std::string_view) { return QueueCancelOutcome::Retry; }
    virtual std::string authGeneration(const std::string& environmentId, const Json& config) = 0;
    // Local/preflight checks only. Throw ModelError for a settings failure and report auth separately.
    virtual std::optional<Delivery> preflight(const Command&) { return std::nullopt; }
    virtual Delivery send(const Command&) = 0;
    virtual Delivery reconcile(const Command&) = 0;
    virtual Observation observe(const std::string& environmentId, const Json& config, const RunRef&) = 0;
    // Recover evidence for an already attributed terminal result without waiting on the Run again.
    virtual RunEvidence readRunEvidence(const std::string&, const Json&, const RunRef&, std::string_view) {
        RunEvidence evidence;
        evidence.availability = RunEvidence::Availability::ReportUnavailable;
        evidence.detail = "Run report is unavailable";
        return evidence;
    }
    virtual InterruptOutcome interrupt(const std::string& environmentId, const Json& config, const RunRef&) = 0;
    virtual InterruptOutcome interruptSessionMessage(const std::string& environmentId, const RunRef& run) {
        return interrupt(environmentId, Json::object(), run);
    }
    virtual Excerpt read(const ReadRequest&) { throw std::runtime_error("Session reading is unavailable"); }
    virtual std::vector<T3Project> projects(std::string_view) { return {}; }
    virtual std::vector<T3Project> projects(const T3EnvironmentRoute& route) { return projects(route.environmentId); }
    virtual T3EnvironmentLinks linkedEnvironments(std::string_view) { return {}; }
    virtual std::pair<std::vector<std::string>, std::optional<Id>> projectSessions(std::string_view, std::string_view, Id,
                                                                                   SessionReadDeadline) {
        return {};
    }
    virtual std::optional<SessionMetadata> sessionMetadata(const SessionTarget&, bool, SessionReadDeadline = {}) { return {}; }
    virtual std::optional<SessionEventPage> sessionEvents(const SessionTarget&, const SessionEventFilter&) { return {}; }
    virtual std::optional<SessionEvent> sessionEvent(const SessionTarget&, std::string_view) { return {}; }
    virtual std::optional<SessionCounts> sessionCounts(const SessionTarget&) { return {}; }
    virtual std::optional<SessionAttachment> sessionAttachment(const SessionTarget&, std::string_view, std::string_view) { return {}; }
};
}
