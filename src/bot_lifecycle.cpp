#include "storage.hpp"

namespace flockv0 {
bool Store::Impl::botDeleted(std::string_view bot) const {
    return query("SELECT 1 FROM deleted_bots WHERE bot=?",{bot}).next();
}

bool Store::botDeleted(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    return p_->botDeleted(bot);
}

void Store::deleteBot(const Person& actor, std::string_view bot) {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    auto permissions = policy();
    if (!permissions.allows(actor,Action::bot_settings,bot)) throw DecisionDenied{};
    if (p_->query(R"SQL(
SELECT 1 WHERE
  EXISTS (SELECT 1 FROM tasks WHERE bot_id=?1 AND state NOT IN ('completed','cancelled'))
  OR EXISTS (SELECT 1 FROM messages m JOIN conversation_turns t ON t.message_id=m.id
             WHERE m.bot_id=?1 AND t.status<>'done')
  OR EXISTS (SELECT 1 FROM workspace_commands WHERE bot=?1 AND state IN ('running','stop_requested'))
  OR EXISTS (SELECT 1 FROM session_message_waits WHERE bot=?1 AND notified=0)
  OR EXISTS (SELECT 1 FROM messages m JOIN channel_deliveries d ON d.source_id=m.id
             WHERE m.bot_id=?1 AND d.state IN ('pending','unknown'))
)SQL",{bot}).next() || p_->actionablePrReviews(bot).next()) throw BotBusy{};
    p_->exec("INSERT INTO deleted_bots VALUES(?)",{bot});
    p_->retireAutomations(Json::array({bot}));
    permissions.audit(actor,bot,"delete_bot",Json::object());
    tx.commit();
}
}
