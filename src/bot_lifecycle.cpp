#include "storage.hpp"
#include "bot_lifecycle_storage.hpp"
#include "database.hpp"
#include "conversation_storage.hpp"
#include "journal_conversation.hpp"
#include "flock/types.hpp"
#include "flock/person.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"

#include <functional>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
bool BotLifecycleStorage::botDeleted(std::string_view bot) const {
    return db_.query("SELECT 1 FROM deleted_bots WHERE bot=?", {bot}).next();
}

bool Store::botDeleted(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    return p_->botLifecycle.botDeleted(bot);
}

bool BotLifecycleStorage::botPaused(std::string_view bot) const {
    return db_.query("SELECT 1 FROM bot_pauses WHERE bot=?", {bot}).next();
}

bool Store::botPaused(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    return p_->botLifecycle.botPaused(bot);
}

void Store::setBotPaused(const Person& actor, std::string_view bot, bool paused, const std::function<Seconds()>& clock) {
    const SessionDispatchFileLock dispatch(p_->path, SessionDispatchFileLock::Mode::exclusive);
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    auto permissions = policy();
    if (!permissions.allows(actor, Action::bot_settings, bot)) throw DecisionDenied{};
    if (p_->botLifecycle.botPaused(bot) == paused) return;

    if (paused) {
        p_->exec("INSERT INTO bot_pauses VALUES(?)", {bot});
        auto turns = p_->query(R"SQL(
SELECT id FROM messages WHERE bot_id=? AND id IN (
    SELECT message_id FROM conversation_turns WHERE status<>'done')
)SQL",
                               {bot});
        std::vector<Id> openTurns;
        while (turns.next()) openTurns.push_back(turns.number(0));
        for (const auto id : openTurns)
            p_->finishTurn(p_->loadTurnMessage(id), botPausedMessage, nullptr, ConversationReplyRoute::bot_paused);
        p_->exec(R"SQL(
UPDATE workflows SET stop_requested=1 WHERE bot_id=? AND stop_requested=0 AND EXISTS (
    SELECT 1 FROM workflow_stages s LEFT JOIN tasks t ON t.id=s.task_id
    WHERE s.workflow_id=workflows.id AND
      (t.state NOT IN ('completed','cancelled') OR (s.task_id IS NULL AND s.status='pending')))
)SQL",
                 {bot});
        p_->exec(R"SQL(
UPDATE workflow_stages SET status='stopped' WHERE status='pending' AND task_id IS NULL
AND workflow_id IN (SELECT id FROM workflows WHERE bot_id=?)
)SQL",
                 {bot});
        auto tasks = p_->query(R"SQL(
SELECT id FROM tasks WHERE bot_id=? AND state NOT IN ('completed','cancelled','cancelling') ORDER BY id
)SQL",
                               {bot});
        std::vector<Id> openTasks;
        while (tasks.next()) openTasks.push_back(tasks.number(0));
        for (const auto id : openTasks) {
            auto task = p_->taskRecords.load(id);
            if (terminal(task) || task.state == TaskState::cancelling) continue;
            task.cancelledByBotPause = true;
            p_->taskOperations.requestCancellation(task);
        }
        p_->settlePolicyApprovals();
        p_->stopPausedBotParticipation(bot);
        p_->dropPausedBotDeliveries(bot);
    } else {
        p_->automationStorage.skipDueAutomations(bot, clock());
        p_->gitStorage.skipPendingGitWebhooks(bot);
        p_->exec("DELETE FROM bot_pauses WHERE bot=?", {bot});
    }

    permissions.audit(actor, bot, paused ? "pause_bot" : "resume_bot", Json::object());
    tx.commit();
}

void Store::deleteBot(const Person& actor, std::string_view bot) {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    auto permissions = policy();
    if (!permissions.allows(actor, Action::bot_settings, bot)) throw DecisionDenied{};
    if (p_->query(R"SQL(
SELECT 1 WHERE
  EXISTS (SELECT 1 FROM tasks WHERE bot_id=?1 AND state NOT IN ('completed','cancelled'))
  OR EXISTS (SELECT 1 FROM messages m JOIN conversation_turns t ON t.message_id=m.id
             WHERE m.bot_id=?1 AND t.status<>'done')
  OR EXISTS (SELECT 1 FROM workflows w JOIN workflow_stages s ON s.workflow_id=w.id
             WHERE w.bot_id=?1 AND w.stop_requested=0 AND s.task_id IS NULL AND s.status='pending')
  OR EXISTS (SELECT 1 FROM session_message_waits WHERE bot=?1 AND (notified=0 OR interrupt_at IS NOT NULL))
  OR EXISTS (SELECT 1 FROM messages m JOIN session_message_sends s ON s.message_id=m.id
             WHERE m.bot_id=?1 AND s.interrupt_at IS NOT NULL)
  OR EXISTS (SELECT 1 FROM messages m JOIN channel_deliveries d ON d.source_id=m.id
             WHERE m.bot_id=?1 AND d.state IN ('pending','unknown'))
)SQL",
                  {bot})
            .next() ||
        p_->inboxStorage.actionablePrReviews(bot).next())
        throw BotBusy{};
    p_->exec("INSERT INTO deleted_bots VALUES(?)", {bot});
    p_->exec("DELETE FROM bot_pauses WHERE bot=?", {bot});
    p_->automationStorage.retireAutomations(Json::array({bot}));
    permissions.audit(actor, bot, "delete_bot", Json::object());
    tx.commit();
}
}
