#include "policy_internal.hpp"
#include "session_dispatch_lock.hpp"
#include "flock/bot_lifecycle.hpp"
#include "flock/decision_errors.hpp"
#include "store_access.hpp"
#include "bot_lifecycle_storage.hpp"
#include "database.hpp"
#include "conversation_storage.hpp"
#include "journal_conversation.hpp"
#include "flock/types.hpp"
#include "flock/person.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/policy.hpp"
#include "policy_approvals.hpp"
#include "channel_storage.hpp"
#include "inbox_storage.hpp"
#include "session_participation_storage.hpp"
#include "task_records.hpp"

#include <functional>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
bool BotLifecycleStorage::botDeleted(std::string_view bot) const {
    return db_.query("SELECT 1 FROM deleted_bots WHERE bot=?", {bot}).next();
}

bool BotLifecycle::botDeleted(std::string_view bot) const {
    auto& storage = StoreAccess::get(store_);
    std::lock_guard lock(storage.database.mutex);
    return storage.botLifecycle.botDeleted(bot);
}

bool BotLifecycleStorage::botPaused(std::string_view bot) const {
    return db_.query("SELECT 1 FROM bot_pauses WHERE bot=?", {bot}).next();
}

bool BotLifecycle::botPaused(std::string_view bot) const {
    auto& storage = StoreAccess::get(store_);
    std::lock_guard lock(storage.database.mutex);
    return storage.botLifecycle.botPaused(bot);
}

void BotLifecycle::setBotPaused(const Person& actor, std::string_view bot, bool paused, const std::function<Seconds()>& clock) {
    auto& storage = StoreAccess::get(store_);
    const SessionDispatchFileLock dispatch(storage.path, SessionDispatchFileLock::Mode::exclusive);
    std::lock_guard lock(storage.database.mutex);
    Transaction tx(storage.database.db);
    Policy permissions{store_};
    if (!permissions.allows(actor, Action::bot_settings, bot)) throw DecisionDenied{};
    if (storage.botLifecycle.botPaused(bot) == paused) return;

    if (paused) {
        storage.database.exec("INSERT INTO bot_pauses VALUES(?)", {bot});
        auto turns = storage.database.query(R"SQL(
SELECT id FROM messages WHERE bot_id=? AND id IN (
    SELECT message_id FROM conversation_turns WHERE status<>'done')
)SQL",
                                            {bot});
        std::vector<Id> openTurns;
        while (turns.next()) openTurns.push_back(turns.number(0));
        for (const auto id : openTurns)
            storage.conversationTurns.finishTurn(storage.conversationTurns.loadTurnMessage(id), botPausedMessage, nullptr,
                                                 ConversationReplyRoute::bot_paused);
        auto tasks = storage.database.query(R"SQL(
SELECT id FROM tasks WHERE bot_id=? AND state NOT IN ('completed','cancelled','cancelling') ORDER BY id
)SQL",
                                            {bot});
        std::vector<Id> openTasks;
        while (tasks.next()) openTasks.push_back(tasks.number(0));
        for (const auto id : openTasks) {
            auto task = storage.taskRecords.load(id);
            if (terminal(task) || task.state == TaskState::cancelling) continue;
            task.cancelledByBotPause = true;
            storage.taskOperations.requestCancellation(task);
        }
        storage.policyApprovals.settlePolicyApprovals();
        storage.sessionParticipationStorage.stopPausedBotParticipation(bot);
        storage.channelStorage.dropPausedBotDeliveries(bot);
    } else {
        storage.automationStorage.skipDueAutomations(bot, clock());
        storage.database.exec("DELETE FROM bot_pauses WHERE bot=?", {bot});
    }

    policy_storage::audit(storage.database, actor, bot, paused ? "pause_bot" : "resume_bot", Json::object());
    tx.commit();
}

void BotLifecycle::deleteBot(const Person& actor, std::string_view bot) {
    auto& storage = StoreAccess::get(store_);
    std::lock_guard lock(storage.database.mutex);
    Transaction tx(storage.database.db);
    Policy permissions{store_};
    if (!permissions.allows(actor, Action::bot_settings, bot)) throw DecisionDenied{};
    if (storage.database
            .query(R"SQL(
SELECT 1 WHERE
  EXISTS (SELECT 1 FROM tasks WHERE bot_id=?1 AND state NOT IN ('completed','cancelled'))
  OR EXISTS (SELECT 1 FROM messages m JOIN conversation_turns t ON t.message_id=m.id
             WHERE m.bot_id=?1 AND t.status<>'done')
  OR EXISTS (SELECT 1 FROM session_message_waits WHERE bot=?1 AND (notified=0 OR interrupt_at IS NOT NULL))
  OR EXISTS (SELECT 1 FROM messages m JOIN session_message_sends s ON s.message_id=m.id
             WHERE m.bot_id=?1 AND s.interrupt_at IS NOT NULL)
  OR EXISTS (SELECT 1 FROM messages m JOIN channel_deliveries d ON d.source_id=m.id
             WHERE m.bot_id=?1 AND d.state IN ('pending','unknown'))
)SQL",
                   {bot})
            .next())
        throw BotBusy{};
    storage.database.exec("INSERT INTO deleted_bots VALUES(?)", {bot});
    storage.database.exec("DELETE FROM bot_pauses WHERE bot=?", {bot});
    storage.automationStorage.retireAutomations(Json::array({bot}));
    policy_storage::audit(storage.database, actor, bot, "delete_bot", Json::object());
    tx.commit();
}
}
