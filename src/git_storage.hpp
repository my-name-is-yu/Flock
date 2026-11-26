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
class InboxStorage;
class JournalStorage;
class SecretStorage;
class TaskRecords;
struct Decision;
struct GitContext;
struct GitOpenPr;
struct GitPullRequest;
struct GitRepository;
struct GitWebhook;
struct InboxTarget;
struct TaskData;
struct WebIdentityState;

namespace conversation {
struct Message;
}

class PolicyApprovals;
class SessionParticipationStorage;
class GitStorage {
public:
    GitStorage(Database& database, SecretStorage& secretStorage, InboxStorage& inboxStorage, TaskRecords& taskRecords,
               JournalStorage& journalStorage, const WebIdentityState& webIdentity, Policy& permissions,
               ConversationStorage& conversationTurns, const BotLifecycleStorage& botLifecycle, PolicyApprovals& policyApprovals,
               SessionParticipationStorage& sessionParticipationStorage)
        : database(database), secretStorage(secretStorage), inboxStorage(inboxStorage), taskRecords(taskRecords),
          journalStorage(journalStorage), webIdentity(webIdentity), permissions(permissions), conversationTurns(conversationTurns),
          policyApprovals(policyApprovals), sessionParticipationStorage(sessionParticipationStorage), botLifecycle(botLifecycle) {}

    Json gitTool(GitHost&, std::string_view bot, Id task, std::string_view environment, std::string_view project, const Decision&,
                 std::string_view invocation, std::optional<Id> generation = {}, Id turn = 0);
    void projectTaskGitReads(GitHost&, const TaskData&, Json& event);
    Json gitConversationTool(GitHost*, const conversation::Message&, const Decision&, std::string_view key);
    bool gitEffectStarted(std::string_view invocation, Id taskId = 0) const;
    void deliverGitWebhooks();
    void skipPendingGitWebhooks(std::string_view bot);
    bool canReceiveGitWebhook(std::string_view bot, const Json& event);
    bool receiveGitWebhook(const GitWebhook&);
    void decideGitPr(const InboxTarget&, std::string_view decision, const Json& data, const TaskData&);

    struct GitPrFollowup {
        enum class Status { available, attempted, limitReached };
        SessionTarget session;
        PolicyOrigin origin;
        std::string operation;
        Json target;
        Status status;
    };
    std::optional<GitPrFollowup> gitPrFollowup(const conversation::Message&, const IntakeResolver&);
    bool currentGitPrCheck(GitHost&, const conversation::Message&);
    std::string gitPrFollowupInstruction(const conversation::Message&, const GitPrFollowup&, const Decision&) const;
    void recordGitPrFollowup(const conversation::Message&, const GitPrFollowup&);
private:
    enum class GitWatchStart { operationIntent, mutationAdmission };
    struct GitPrKey {
        std::string host, repo;
        Id number;
        auto operator<=>(const GitPrKey&) const = default;
    };
    class GitPrMutationClaim;

    static Json gitJournal(const GitContext&, std::string_view tool, const GitRepository&, Json result);
    Json saveTaskGitRead(Id task, std::string_view invocation, std::string_view tool, const Json& target, const Json& result);
    Json openGitPr(GitHost&, const GitContext&, const GitOpenPr&, std::string_view invocation, bool newIntent, std::string_view reason);
    Id latestGitWebhookId() const;
    bool gitPrWatchUnavailable(const GitRepository&, Id pr, Id task) const;
    bool watchGitPr(Id task, std::string_view bot, const Json& run, Id deliveryCursor, const GitPullRequest&,
                    GitWatchStart = GitWatchStart::operationIntent);
    void gitInboxItem(const TaskData&, std::string_view key, std::string_view url, std::string_view title, std::string_view prompt);

    Database& database;
    SecretStorage& secretStorage;
    InboxStorage& inboxStorage;
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
