#pragma once

#include "policy_origin.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/session_ref.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>
#include <compare>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace flockv0 {
class Policy;
class ConversationStorage;

class BotLifecycleStorage;
class Database;
class GitHost;
class JournalStorage;
class SecretStorage;
class TaskRecords;
struct Decision;
struct GitContext;
struct GitOpenPr;
struct GitRepository;
struct GitWebhook;
struct TaskData;
struct WebIdentityState;

namespace conversation {
struct Message;
}

class PolicyApprovals;
class SessionParticipationStorage;
class GitStorage {
public:
    GitStorage(Database& database, SecretStorage& secretStorage, TaskRecords& taskRecords, JournalStorage& journalStorage,
               const WebIdentityState& webIdentity, Policy& permissions, ConversationStorage& conversationTurns,
               const BotLifecycleStorage& botLifecycle, PolicyApprovals& policyApprovals,
               SessionParticipationStorage& sessionParticipationStorage)
        : database(database), secretStorage(secretStorage), taskRecords(taskRecords), journalStorage(journalStorage),
          webIdentity(webIdentity), permissions(permissions), conversationTurns(conversationTurns), policyApprovals(policyApprovals),
          sessionParticipationStorage(sessionParticipationStorage), botLifecycle(botLifecycle) {}

    Json gitTool(GitHost&, std::string_view bot, Id task, std::string_view environment, std::string_view project, const Decision&,
                 std::string_view invocation, std::optional<Id> generation = {}, Id turn = 0);
    void projectTaskGitReads(GitHost&, const TaskData&, Json& event);
    void projectExternalCheck(GitHost*, const TaskData&, Json& event);
    Json gitConversationTool(GitHost*, const conversation::Message&, const Decision&, std::string_view key);
    bool gitEffectStarted(std::string_view invocation, Id taskId = 0) const;
    void deliverGitWebhooks();
    bool canReceiveGitWebhook(std::string_view bot, const Json& event);
    bool receiveGitWebhook(const GitWebhook&);

    struct GitPrFollowup {
        enum class Status { available, attempted };
        SessionTarget session;
        PolicyOrigin origin;
        std::string operation;
        Json target;
        Status status;
    };
    std::optional<GitPrFollowup> gitPrFollowup(const conversation::Message&, const IntakeResolver&);
    bool gitPrReadyForFollowup(GitHost&, const conversation::Message&);
    std::string gitPrFollowupInstruction(const conversation::Message&, const GitPrFollowup&, const Decision&) const;
    void recordGitPrFollowup(const conversation::Message&, const GitPrFollowup&);
    Json applyGitTaskDecision(const conversation::Message&, const Decision&);
    Json watchedPullRequests(const conversation::Message&);
    bool gitNotificationCanComplete(const conversation::Message&);
private:
    struct GitPrKey {
        std::string host, repo;
        Id number;
        auto operator<=>(const GitPrKey&) const = default;
    };
    class GitPrMutationClaim;

    static Json gitJournal(const GitContext&, std::string_view tool, const GitRepository&, Json result);
    Json saveTaskGitRead(Id task, std::string_view invocation, std::string_view tool, const Json& target, const Json& result);
    Json openGitPr(GitHost&, const GitContext&, const GitOpenPr&, std::string_view invocation, bool newIntent, std::optional<Id> prNumber,
                   std::string_view reason);
    Id latestGitWebhookId() const;
    bool gitPrWatchUnavailable(const GitRepository&, Id pr, Id task) const;
    enum class GitWatchResult { attached, taskEnded, unavailable };
    GitWatchResult watchGitPr(const TaskData&, const Json& run, Id deliveryCursor, const GitRepository&, Id pr, std::string_view url,
                              std::string_view headSha);

    Database& database;
    SecretStorage& secretStorage;
    TaskRecords& taskRecords;
    JournalStorage& journalStorage;
    const WebIdentityState& webIdentity;
    Policy& permissions;
    ConversationStorage& conversationTurns;
    PolicyApprovals& policyApprovals;
    SessionParticipationStorage& sessionParticipationStorage;
    const BotLifecycleStorage& botLifecycle;
    std::optional<Id> gitWebhooksDrainedAt;
    std::multiset<GitPrKey> gitPrMutationsInFlight, gitFeedbackMutationsInFlight;
};
}
