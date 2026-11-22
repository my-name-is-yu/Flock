#include "flock/root_delivery.hpp"
#include "storage.hpp"

#include <algorithm>
#include <array>

namespace flockv0 {
namespace {

using State = RootDeliveryState;

const ReturnChannel& channelOf(const Participation& participation) {
    return std::visit([](const auto& entry) -> const ReturnChannel& { return entry.channel; },participation);
}

RootDeliveryView deliveryView(const Statement& row) {
    RootDeliveryView result{row.string(0),static_cast<State>(row.number(1)),{},row.string(5)};
    if (!row.string(2).empty())
        result.receipt = ThreadReceipt{row.string(2),row.string(3),row.string(4)};
    return result;
}

bool validReceipt(const ThreadReceipt& receipt) {
    return !receipt.messageId.empty() && !receipt.runId.empty() &&
           (receipt.mode == "queued" || receipt.mode == "started");
}

}

std::string_view rootDeliveryStateName(RootDeliveryState state) {
    constexpr std::array<std::string_view,8> names{
        "prepared","dispatching","accepted","queued","observed","failed","rejected","unknown"
    };
    return names.at(static_cast<std::size_t>(state));
}

void Store::Impl::initializeRootDeliverySchema() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS thread_deliveries(
 command_id TEXT PRIMARY KEY, channel_id INTEGER NOT NULL REFERENCES session_controls(channel_id),
 generation INTEGER NOT NULL, text TEXT NOT NULL, state INTEGER NOT NULL DEFAULT 0,
 message_id TEXT NOT NULL DEFAULT '', run_id TEXT NOT NULL DEFAULT '', mode TEXT NOT NULL DEFAULT '',
 observed_run_id TEXT NOT NULL DEFAULT '', cursor INTEGER NOT NULL, input_position INTEGER,
 claimed_at INTEGER)
)SQL");
    exec("CREATE INDEX IF NOT EXISTS thread_deliveries_channel ON thread_deliveries(channel_id,state)");
}

void RootDelivery::prepare(Id channel, Id generation, std::string_view commandId, std::string_view bot,
                           std::string_view requestId, std::string_view text, Id observationCursor) {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    const auto outward = "[Flock Bot " + std::string(bot) + " | request " + std::string(requestId) +
                         " | action " + std::string(commandId) + "]\n" + redactPairingCodes(db.sharedRedactor().text(text));
    auto existing = db.query("SELECT channel_id,generation,text FROM thread_deliveries WHERE command_id=?",{commandId});
    if (existing.next()) {
        if (existing.number(0) != channel || existing.number(1) != generation || existing.string(2) != outward)
            throw Conflict("This Session send already has a different payload");
        return;
    }
    db.exec(R"SQL(
INSERT INTO thread_deliveries(command_id,channel_id,generation,text,cursor)
SELECT ?,channel_id,generation,?,? FROM session_controls WHERE channel_id=? AND generation=? AND state='active'
)SQL",{commandId,outward,observationCursor,channel,generation});
    if (!sqlite3_changes(db.db)) throw Conflict("Session sending is paused or has stopped");
}

RootDeliveryView RootDelivery::deliver(const Person& person, Id channel, std::string_view commandId) {
    auto& db = *store_.p_;
    ThreadTarget target;
    std::string auth, text;
    bool coordination;
    Id cursor;
    auto rowFor = [&] {
        return db.query(R"SQL(
SELECT d.command_id,d.state,d.message_id,d.run_id,d.mode,d.observed_run_id,d.text,d.cursor,s.auth_generation
FROM thread_deliveries d JOIN session_controls s ON s.channel_id=d.channel_id
WHERE d.channel_id=? AND d.command_id=?
)SQL",{channel,commandId});
    };
    {
        std::lock_guard lock(db.mutex);
        const auto participation = db.authorizeParticipation(person,channel);
        target = channelOf(participation).origin.invitation.target;
        coordination = std::holds_alternative<CoordinationActivation>(participation);
        auto row = rowFor();
        if (!row.next()) throw Conflict("Unknown Session send");
        if (row.number(1) != static_cast<int>(State::Prepared)) return deliveryView(row);
        text = row.string(6);
        cursor = row.number(7);
        auth = row.string(8);
    }
    const auto page = coordination ? session_.dispatchHistory(target,cursor) : session_.read(target,std::nullopt);
    if (!page) return {std::string(commandId),State::Prepared,{},{}};
    if (coordination && page->cursor < cursor)
        return {std::string(commandId),State::Prepared,{},{}};
    const bool humanInput = coordination && std::ranges::any_of(page->events,[&](const auto& event) {
        return event.position > cursor && event.kind == "user_message" && event.createdBy == "user";
    });
    const bool reject = humanInput || page->target != target || page->authGeneration != auth;
    {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        db.authorizeParticipation(person,channel);
        db.exec(R"SQL(
UPDATE thread_deliveries SET state=?,claimed_at=CASE WHEN ? THEN NULL ELSE ? END
WHERE channel_id=? AND command_id=? AND state=? AND EXISTS(
 SELECT 1 FROM session_controls s WHERE s.channel_id=thread_deliveries.channel_id AND s.state='active'
 AND s.generation=thread_deliveries.generation)
)SQL",{static_cast<int>(reject ? State::Rejected : State::Dispatching),static_cast<int>(reject),unixTime(),channel,commandId,
        static_cast<int>(State::Prepared)});
        const bool claimed = sqlite3_changes(db.db) != 0;
        tx.commit();
        if (!claimed || reject) {
            auto row = rowFor();
            if (!row.next()) throw Conflict("Unknown Session send");
            return deliveryView(row);
        }
    }

    ThreadSend outcome;
    try { outcome = session_.send(target,auth,commandId,text); }
    catch (const std::exception&) { outcome = {}; }
    State next = State::Unknown;
    if (outcome.kind == ThreadSend::Rejected) next = State::Rejected;
    if (outcome.kind == ThreadSend::Accepted && outcome.receipt && validReceipt(*outcome.receipt))
        next = outcome.receipt->mode == "queued" ? State::Queued : State::Accepted;
    else outcome.receipt.reset();

    std::lock_guard lock(db.mutex);
    const auto receipt = outcome.receipt.value_or(ThreadReceipt{});
    db.exec(R"SQL(
UPDATE thread_deliveries SET state=?,message_id=?,run_id=?,mode=? WHERE command_id=? AND state=?
)SQL",{static_cast<int>(next),receipt.messageId,receipt.runId,receipt.mode,commandId,
        static_cast<int>(State::Dispatching)});
    auto row = rowFor();
    if (!row.next()) throw Conflict("Unknown Session send");
    return deliveryView(row);
}

void RootDelivery::reconcile(const Person& person, Id channel) {
    auto& db = *store_.p_;
    struct Pending {
        std::string command, text, message, observedRun;
        State state;
        Id cursor, inputPosition;
    };
    ThreadTarget target;
    std::vector<Pending> pending;
    {
        std::lock_guard lock(db.mutex);
        const auto participation = db.authorizeParticipation(person,channel,false);
        target = channelOf(participation).origin.invitation.target;
        auto rows = db.query(R"SQL(
SELECT d.command_id,d.text,d.message_id,d.observed_run_id,d.state,d.cursor,d.input_position
FROM thread_deliveries d
WHERE d.channel_id=? AND d.state IN (?,?,?) ORDER BY d.rowid LIMIT 16
)SQL",{channel,static_cast<int>(State::Accepted),static_cast<int>(State::Queued),static_cast<int>(State::Unknown)});
        while (rows.next())
            pending.push_back({rows.string(0),rows.string(1),rows.string(2),rows.string(3),
                               static_cast<State>(rows.number(4)),rows.number(5),rows.number(6)});
    }
    for (auto& delivery : pending) {
        const auto page = session_.read(target,delivery.cursor);
        if (!page || page->target != target || page->cursor < delivery.cursor) continue;
        // History is read-only. Even after reauthentication it cannot create a
        // duplicate send, and only this Bot's immutable MCP input can correlate.
        auto state = delivery.state;
        const auto header = delivery.text.substr(0,delivery.text.find('\n'));
        for (const auto& event : page->events) {
            if (event.kind == "user_message" && event.createdBy == "agent" && event.creationSource == "mcp" &&
                ((!delivery.message.empty() && event.messageId == delivery.message) ||
                 (delivery.message.empty() && event.text.starts_with(header + "\n")))) {
                delivery.observedRun = event.runId;
                delivery.inputPosition = event.position;
            } else if (event.kind == "assistant_message" && !event.text.empty() && !delivery.observedRun.empty() &&
                       event.runId == delivery.observedRun && event.position > delivery.inputPosition) {
                if (event.status == "completed") state = State::Observed;
                else if (event.status == "failed" || event.status == "cancelled" || event.status == "interrupted")
                    state = State::Failed;
                if (state != delivery.state) break;
            }
        }
        std::lock_guard lock(db.mutex);
        db.exec(R"SQL(
UPDATE thread_deliveries SET state=?,cursor=?,observed_run_id=?,input_position=?
WHERE command_id=? AND state=? AND cursor=?
)SQL",{static_cast<int>(state),page->cursor,
        delivery.observedRun,delivery.inputPosition,delivery.command,static_cast<int>(delivery.state),delivery.cursor});
    }
}

}
