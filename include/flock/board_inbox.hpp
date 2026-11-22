#pragma once
#include "flock/access.hpp"

namespace flockv0 {
enum class InboxKind {
    user_question, extra_run, deadline, model_budget, conversation_model_budget,
    unknown_delivery, grant_request, protected_input, bot_change, pr_review, pr_reconciliation
};

std::string_view inboxKindName(InboxKind);
InboxKind parseInboxKind(std::string_view);
Role inboxDecisionRole(InboxKind);

struct InboxTarget {
    std::string botId;
    InboxKind itemKind;
    Id itemId, waitId, taskId, targetRevision, targetGeneration;
    bool operator==(const InboxTarget&) const = default;
};

struct RequestChanged : Conflict {
    RequestChanged() : Conflict("The request changed. Refresh the inbox and review the current request.") {}
};

struct DecisionUnavailable : Conflict {
    DecisionUnavailable() : Conflict("The request was already decided or closed.") {}
};

struct DecisionDenied : Conflict {
    DecisionDenied() : Conflict("You do not have permission to decide this request.") {}
};
}
