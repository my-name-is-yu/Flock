#include "journal_storage.hpp"
#include "journal_git.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "database.hpp"

#include <nlohmann/json.hpp>

namespace flockv0 {
Id JournalStorage::journal(Id task, const GitPrFollowupEntry& entry) {
    return database.writeJournal(task, JournalKind::git_pr_followup_requested,
                                 {{"operationId", entry.operationId}, {"eventId", entry.eventId}, {"messageId", entry.messageId}},
                                 {.gitEventId = entry.eventId, .operationId = entry.operationId});
}
}
