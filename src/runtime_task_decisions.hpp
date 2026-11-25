#pragma once

#include "journal_task.hpp"
#include "storage.hpp"

namespace flockv0 {
class Runtime::TaskDecisions {
public:
    TaskDecisions(Store::Impl& db, TaskUpdates& updates, RunReports& reports, SessionHistory& history, PeerExchanges& peerExchanges,
                  OutboxDispatch& dispatch, const std::string& sessionRead, GitHost* gitHost);

    bool canComplete(const TaskData& task, const Json& event) const;
    void rejectDecision(const TaskData& task, const Json& event, const TaskDecision& decision, std::string_view message,
                        bool replay = false);
    void accept(const TaskData& snapshot, const Json& event, const TaskDecision& decision, std::string_view approvedInvocation = {});
    bool continueStage(const TaskData& task);
    bool resumeGitApproval(const TaskData& task);
private:
    void acceptReplyOrCompletion(const TaskData& snapshot, const Json& event, const TaskDecision& decision);
    void complete(TaskData& current, const Json& event, const TaskDecision& decision);
    CompletionProvenance completionProvenance(const TaskData& task, const Json& event);

    Store::Impl& db;
    TaskUpdates& updates;
    RunReports& reports;
    SessionHistory& history;
    PeerExchanges& peerExchanges;
    OutboxDispatch& dispatch;
    const std::string& sessionRead;
    GitHost* gitHost;
};
}
