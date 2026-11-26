#include "peer_storage.hpp"
#include "database.hpp"
#include "journal_peer.hpp"
#include "flock/task.hpp"

#include <mutex>

#include <nlohmann/json.hpp>

namespace flockv0 {
std::optional<PeerExchange> PeerStorage::peerExchange(Id id) const {
    auto rows = db_.query("SELECT id,kind,phase,last_message,sender_task,recipient_task FROM peer_exchanges"
                          " WHERE phase<>'closed' AND (sender_task=? OR recipient_task=?)",
                          {id, id});
    if (!rows.next()) return std::nullopt;
    return PeerExchange{rows.string(0), rows.string(1), rows.string(2), rows.string(3), rows.number(4), rows.number(5)};
}

bool PeerStorage::peerWorkRouteWouldCycle(Id sender, std::string_view environment, std::string_view project) const {
    constexpr auto sql = R"SQL(
WITH RECURSIVE source(env,project) AS (
  SELECT environment_id,project_id FROM tasks WHERE id=?1
), work_routes(source_env,source_project,current_env,current_project,pending_env,pending_project) AS (
  SELECT s.environment_id,s.project_id,r.environment_id,r.project_id,
         CASE WHEN r.state<>'cancelling' THEN r.pending_environment_id END,r.pending_project_id
  FROM peer_exchanges p JOIN tasks s ON s.id=p.sender_task JOIN tasks r ON r.id=p.recipient_task
  WHERE p.kind='work' AND p.phase<>'closed' AND p.sender_task<>?1
), dependencies(source_env,source_project,target_env,target_project) AS (
  SELECT source_env,source_project,current_env,current_project FROM work_routes
  WHERE current_project<>''
  UNION ALL
  SELECT source_env,source_project,pending_env,pending_project FROM work_routes
  WHERE pending_env IS NOT NULL AND pending_project<>''
), reachable(env,project) AS (
  SELECT ?2,?3 FROM source WHERE env<>?2 OR project<>?3
  UNION
  SELECT target_env,target_project FROM dependencies JOIN reachable
    ON source_env=env AND source_project=project
)
SELECT 1 FROM reachable JOIN source USING(env,project) LIMIT 1
)SQL";
    return db_.query(sql, {sender, environment, project}).next();
}

bool PeerStorage::recipientRouteWouldCycle(Id task, std::string_view environment, std::string_view project) const {
    if (project.empty()) return false;
    const auto peer = peerExchange(task);
    return peer && peer->recipient == task && peer->kind == "work" && peerWorkRouteWouldCycle(peer->sender, environment, project);
}

bool PeerStorage::replanReady(const TaskData& task) const {
    if (!task.replanning) return false;
    std::lock_guard lock(db_.mutex);
    const Id id = task.id;
    const auto peer = peerExchange(id);
    return !peer || peer->allowsReplan(id);
}

void PeerStorage::peerMessage(const PeerExchange& exchange, const BotMessageEntry& entry, std::optional<Id> forwardedSource) {
    const auto data = encodeBotMessage(entry);
    const JournalKeys keys{.exchangeId = entry.exchangeId, .peerMessageId = entry.messageId, .forwardedSourceMessageId = forwardedSource};
    db_.writeJournal(exchange.sender, JournalKind::bot_message, data, keys);
    db_.writeJournal(exchange.recipient, JournalKind::bot_message, data, keys);
}

}
