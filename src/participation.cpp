#include "flock/participation.hpp"
#include "flock/limits.hpp"
#include "connector_credentials.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <future>
#include <sys/file.h>
#include <unistd.h>

namespace flockv0 {
namespace {

constexpr std::string_view tokenPrefix = "flock-t3-";
constexpr std::size_t maxInvitationNoteBytes = 8'192;
using connector_credentials::digest;

// An invitation gives the inviter's stop authority over the invitee in this Session.
// Follow outgoing edges only. UNION also makes repeated invitations and cycles harmless.
const std::string stopCascade = R"SQL(
WITH RECURSIVE stopping(id) AS (
 SELECT ?1
 UNION
 SELECT i.participation_id FROM session_invitations i
 JOIN stopping s ON s.id=i.inviter_participation_id
 WHERE i.message_id<?2
)
)SQL";

bool validId(std::string_view value, std::size_t max = 1024) {
    return !value.empty() && value.size() <= max && std::ranges::none_of(value, [](unsigned char c) { return c <= 32 || c == 127; });
}

}

SessionDispatchFileLock::SessionDispatchFileLock(const std::filesystem::path& database, Mode mode) {
    const auto lockPath = std::filesystem::canonical(database).string() + ".session-dispatch.lock";
    fd_ = open(lockPath.c_str(), O_CLOEXEC | O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (fd_ < 0) throw Conflict("Could not open the T3 dispatch lock");
    const auto operation = mode == Mode::shared ? LOCK_SH : LOCK_EX;
    int result;
    do {
        result = ::flock(fd_, operation);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        close(fd_);
        throw Conflict("Could not acquire the T3 dispatch lock");
    }
}

SessionDispatchFileLock::~SessionDispatchFileLock() {
    close(fd_);
}

std::string redactT3Tokens(std::string value) {
    return connector_credentials::redact(std::move(value), tokenPrefix, "[T3 connection token]");
}

std::string T3Connections::connect(std::string_view environment, std::string_view person, unsigned port,
                                   const std::function<void(std::string_view)>& install, bool refreshProviders) {
    if (!validId(environment) || !validId(person) || port == 0 || port > 65535) throw Conflict("Invalid T3 connection");
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto rotate = refreshProviders || [&] {
        auto row = db.query("SELECT person_id,port FROM t3_connector");
        return !row.next() || row.string(0) != person || row.number(1) != port ||
               !db.query("SELECT 1 FROM t3_connected_environments LIMIT 1").next();
    }();
    std::string token;
    if (rotate) {
        token = connector_credentials::bearer(tokenPrefix);
        db.exec(R"SQL(
INSERT INTO t3_connector VALUES(1,?,?,?,?)
ON CONFLICT(singleton) DO UPDATE SET id=excluded.id,person_id=excluded.person_id,
 token_digest=excluded.token_digest,port=excluded.port
)SQL",
                {actionId(), person, digest(token), static_cast<Id>(port)});
    }
    db.exec("INSERT OR IGNORE INTO t3_connected_environments VALUES(?)", {environment});
    if (install) install(token);
    tx.commit();
    return token;
}

void T3Connections::rebindPort(unsigned port, const std::function<void(std::string_view)>& install) {
    if (port == 0 || port > 65535 || !install) throw Conflict("Invalid T3 connection port");
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto currentPort = this->port();
    if (currentPort && currentPort != port) {
        const auto token = connector_credentials::bearer(tokenPrefix);
        // The provider can verify its existing registration against the old token until its update succeeds.
        install(token);
        db.exec("UPDATE t3_connector SET id=?,token_digest=?,port=?", {actionId(), digest(token), static_cast<Id>(port)});
    }
    tx.commit();
}

bool T3Connections::ownsBearer(std::string_view token) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    return db.query("SELECT 1 FROM t3_connector WHERE token_digest=?", {digest(token)}).next();
}

bool T3Connections::disconnect(std::string_view environment) {
    auto& db = *store_.p_;
    const SessionDispatchFileLock dispatch(db.path, SessionDispatchFileLock::Mode::exclusive);
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    auto rows = db.query(R"SQL(
SELECT m.id FROM messages m JOIN conversation_turns t ON t.message_id=m.id WHERE t.status<>'done'
 AND json_type(m.routing,'$.sessionWake')='object'
 AND (json_extract(m.routing,'$.sessionWake.environmentId')=?1
      OR json_extract(m.routing,'$.sessionOrigin.environmentId')=?1)
)SQL",
                         {environment});
    std::vector<Id> wakes;
    while (rows.next()) wakes.push_back(rows.number(0));
    for (const auto wake : wakes) db.finishTurn(db.loadTurnMessage(wake), "The T3 connection was disconnected.", nullptr, "session_access");
    db.exec(R"SQL(
UPDATE session_message_waits SET notified=1 WHERE notified=0 AND (environment=?1 OR message_id IN (
 SELECT id FROM messages WHERE json_extract(routing,'$.sessionOrigin.environmentId')=?1
))
)SQL",
            {environment});
    db.exec("DELETE FROM t3_connected_environments WHERE environment_id=?", {environment});
    db.settlePolicyApprovals();
    const bool removeProvider = db.query("SELECT 1 FROM t3_connector WHERE "
                                         "NOT EXISTS(SELECT 1 FROM t3_connected_environments)")
                                    .next();
    tx.commit();
    return removeProvider;
}

bool T3Connections::providerCleanupPending() const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    return db.query("SELECT 1 FROM t3_connector WHERE NOT EXISTS(SELECT 1 FROM t3_connected_environments)").next();
}

void T3Connections::providerRemoved() {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    db.exec("DELETE FROM t3_connector WHERE NOT EXISTS(SELECT 1 FROM t3_connected_environments)");
}

std::optional<T3Connection> T3Connections::authenticate(std::string_view token) const {
    if (!connector_credentials::validBearer(token, tokenPrefix)) return {};
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    auto row = db.query("SELECT id,person_id FROM t3_connector WHERE token_digest=?"
                        " AND EXISTS(SELECT 1 FROM t3_connected_environments)",
                        {digest(token)});
    if (!row.next()) return {};
    return T3Connection{row.string(0), row.string(1)};
}

std::optional<T3ConnectionSettings> T3Connections::settings(std::string_view person, bool installationRoot) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    auto rows = db.query("SELECT port,environment_id FROM t3_connector CROSS JOIN t3_connected_environments"
                         " WHERE (? OR person_id=?) ORDER BY environment_id",
                         {installationRoot ? 1 : 0, person});
    if (!rows.next()) return {};
    T3ConnectionSettings result{static_cast<unsigned>(rows.number(0)), {}};
    do {
        result.environments.push_back(rows.string(1));
    } while (rows.next());
    return result;
}

unsigned T3Connections::port() const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    auto row = db.query("SELECT port FROM t3_connector WHERE EXISTS(SELECT 1 FROM t3_connected_environments)");
    return row.next() ? static_cast<unsigned>(row.number(0)) : 0;
}

bool T3Connections::configured() const {
    return port() != 0;
}

bool Store::Impl::t3EnvironmentConnected(std::string_view environment) {
    std::lock_guard lock(mutex);
    return query("SELECT 1 FROM t3_connected_environments WHERE environment_id=?", {environment}).next();
}

Person Store::Impl::sessionActor(std::string_view person) {
    if (resolveWebIdentity) {
        if (const auto root = resolveWebIdentity(person)) return {std::string(person), *root};
    } else if (person == localOwner.id && !Policy(*this).requiresIdentity())
        return localOwner;
    return {};
}

Person SessionParticipation::person(const T3Connection& connection) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    if (!db.query("SELECT 1 FROM t3_connector WHERE id=? AND person_id=? "
                  "AND EXISTS(SELECT 1 FROM t3_connected_environments)",
                  {connection.id, connection.personId})
             .next())
        throw Conflict("The T3 connection is unavailable");
    const auto actor = db.sessionActor(connection.personId);
    if (actor.id.empty()) throw Conflict("The connected person is unavailable");
    return actor;
}

Id SessionParticipation::ask(const T3Connection& connection, const SessionAsk& request, const IntakeResolver& intake) {
    const auto& [bot, text, thread, requestId] = request;
    if (!validBotId(bot) || !validId(thread, 256) || text.size() > limits::mcp::maxRequestTextBytes || text.find('\0') != text.npos ||
        !conversation_storage::hasText(text) || requestId.empty() || requestId.size() > 128 ||
        requestId.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-") != requestId.npos)
        throw Conflict("Invalid flock_ask arguments");

    auto& db = *store_.p_;
    const auto policy = store_.policy();
    const auto authorizedPerson = [&] {
        const auto actor = person(connection);
        if (!policy.allows(actor, Action::bot_decide, bot)) throw Conflict("Bot access is required");
        return actor;
    };
    const auto fingerprint = digest(Json::array({bot, text, thread}).dump());
    const auto connected = [&](std::string_view environment) {
        return db.query("SELECT 1 FROM t3_connected_environments WHERE environment_id=?", {environment}).next() &&
               policy.allows(bot, Action::environment_inspect, environment);
    };
    const auto replay = [&]() -> Id {
        auto row = db.query(R"SQL(
SELECT a.message_id,a.request_digest,p.environment_id FROM session_asks a
JOIN session_participations p ON p.id=a.participation_id WHERE a.person_id=? AND a.request_id=?
)SQL",
                            {connection.personId, requestId});
        if (!row.next()) return 0;
        if (row.string(1) != fingerprint) throw Conflict("This requestId already has different arguments");
        if (!connected(row.string(2))) throw Conflict("This Session is unavailable through your T3 connection");
        return row.number(0);
    };

    const SessionDispatchFileLock dispatch(db.path, SessionDispatchFileLock::Mode::shared);
    std::vector<std::string> environments;
    {
        std::lock_guard lock(db.mutex);
        authorizedPerson();
        if (const auto message = replay()) return message;
        if (db.botPaused(bot)) throw BotPaused{};
        const auto settings = T3Connections(store_).settings(connection.personId);
        if (settings)
            for (const auto& environment : settings->environments)
                if (connected(environment)) environments.push_back(environment);
    }
    std::optional<SessionOrigin> origin;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::vector<std::future<std::optional<SessionMetadata>>> probes;
    for (const auto& environment : environments) {
        probes.push_back(std::async(std::launch::async,
                                    [&, environment, deadline] { return t3_.sessionMetadata({environment, thread}, false, deadline); }));
    }
    for (std::size_t i = 0; i < environments.size(); ++i) {
        std::optional<SessionMetadata> metadata;
        try {
            metadata = probes[i].get();
        } catch (const std::exception&) {
            continue;
        }
        if (!metadata || metadata->projectId.empty()) continue;
        if (origin) throw Conflict("This threadId matched more than one connected T3 environment");
        origin = SessionOrigin{{environments[i], thread}, std::move(metadata->title)};
    }
    std::lock_guard lock(db.mutex);
    Transaction transaction(db.db);
    const auto actor = authorizedPerson();
    if (const auto message = replay()) return message;
    if (!origin || !connected(origin->target.environmentId)) throw Conflict("This Session is unavailable through your T3 connection");
    if (!policy.allows(bot, Resource::sessionRead(origin->target.environmentId, thread))) origin->title.clear();
    const auto scrub = db.sharedRedactor();
    for (const std::string_view id :
         {std::string_view(bot), std::string_view(origin->target.environmentId), std::string_view(thread), std::string_view(requestId)}) {
        scrub.requirePublic(id);
        if (redactT3Tokens(std::string(id)) != id) throw Conflict("Invalid Session ask identifier");
    }
    origin->title = redactT3Tokens(scrub.text(origin->title));
    db.exec(R"SQL(
INSERT INTO session_participations(bot_id,environment_id,thread_id,person_id,state) VALUES(?,?,?,?,'active')
ON CONFLICT(bot_id,environment_id,thread_id) DO NOTHING
)SQL",
            {bot, origin->target.environmentId, thread, actor.id});
    auto row = db.query("SELECT id,person_id FROM session_participations WHERE bot_id=? AND environment_id=? AND thread_id=?",
                        {bot, origin->target.environmentId, thread});
    if (!row.next() || row.string(1) != actor.id) throw Conflict("This Session participation is unavailable");
    const auto participation = row.number(0);
    db.exec("UPDATE session_participations SET state='active' WHERE id=? AND state<>'active'", {participation});
    const auto safeText = redactT3Tokens(scrub.text(text));
    const auto routing = intake(bot, safeText);
    if (routing.botId != bot) throw Conflict("The Bot is unavailable");
    const auto message =
        db.enqueueConversation(actor, routing, conversation::PrincipalSource::session, "session-ask:" + requestId, safeText, 0, &*origin);
    db.exec("INSERT INTO session_asks VALUES(?,?,?,?,?)", {actor.id, requestId, participation, fingerprint, message});
    transaction.commit();
    return message;
}

bool Store::Impl::inviteSessionBot(const conversation::Message& message, int step, const Decision& decision, T3& t3,
                                   const IntakeResolver& intake, const PeerResolver& peers, std::stop_token shutdown) {
    constexpr Id maxDepth = 3;
    constexpr auto stopped = "This Bot's participation in the Session has stopped";
    constexpr auto unavailable = "This Session is unavailable through your T3 connection";
    const auto replay = [&]() -> Json {
        auto row = query("SELECT message_id,depth FROM session_invitations WHERE source_message_id=? AND step=?", {message.id, step});
        if (!row.next()) return nullptr;
        return {{"ok", true}, {"messageId", row.number(0)}, {"depth", row.number(1)}};
    };
    const auto finish = [&](const auto& apply) {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested()) return false;
        if (!authorizeTurn(message)) {
            transaction.commit();
            return false;
        }
        if (query("SELECT 1 FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL", {message.id, step}).next()) {
            auto result = replay();
            if (result.is_null()) result = apply();
            saveTurnResults(message.id, step, Json::array({result}));
        }
        transaction.commit();
        return true;
    };

    try {
        const auto& args = decision.args;
        using conversation_storage::text;
        if (!args.is_object() || args.size() != 4) throw Conflict("Invalid invite_bot arguments");
        const auto bot = text(args, "botId", 128, true);
        const SessionTarget target{text(args, "environmentId", 256, true), text(args, "threadId", 256, true)};
        const auto note = text(args, "note", maxInvitationNoteBytes, true);
        if (!validBotId(bot) || !validId(target.environmentId, 256) || !validId(target.threadId, 256) || note.find('\0') != note.npos)
            throw Conflict("Invalid invite_bot arguments");
        if (bot == message.bot) throw Conflict("A Bot cannot invite itself");

        struct Sender {
            Person actor;
            Id participation, depth;
        };
        const auto participation = [&](std::string_view id) {
            return query("SELECT id,person_id,state FROM session_participations WHERE bot_id=? AND environment_id=? AND thread_id=?",
                         {id, target.environmentId, target.threadId});
        };
        const auto authorize = [&]() -> Sender {
            const auto actor = turnActor(message);
            Policy policy(*this);
            if (!policy.allows(actor, Action::bot_decide, message.bot) || !policy.allows(actor, Action::bot_decide, bot))
                throw Conflict("Bot access is required");
            if (!peers || std::ranges::none_of(peers(message.bot), [&](const PeerBot& peer) { return peer.intake.botId == bot; }))
                throw Conflict("The destination Bot is unavailable or not allowed");
            if (botPaused(bot)) throw BotPaused{};
            const auto read = Resource::sessionRead(target.environmentId, target.threadId);
            if (!policy.allows(message.bot, read) || !policy.allows(bot, read)) throw Conflict(unavailable);
            if (!policy.allows(message.bot, Action::environment_inspect, target.environmentId) ||
                !policy.allows(bot, Action::environment_inspect, target.environmentId) ||
                !query("SELECT 1 FROM t3_connector CROSS JOIN t3_connected_environments "
                       "WHERE person_id=? AND environment_id=?",
                       {actor.id, target.environmentId})
                     .next())
                throw Conflict(unavailable);
            if (message.routing.contains("sessionOrigin") && !activeSessionOrigin(message)) throw Conflict(stopped);

            auto inviter = participation(message.bot);
            auto depth = message.routing.value("sessionInvitationDepth", Id{0});
            Id inviterId = 0;
            if (inviter.next()) {
                if (sessionParticipationStopped(message.bot, target, message.id)) throw Conflict(stopped);
                if (inviter.string(1) != actor.id) throw Conflict(unavailable);
                inviterId = inviter.number(0);
            } else {
                auto launch = query(R"SQL(
WITH RECURSIVE ancestry(id,source) AS (
 SELECT t.id,json_extract(t.data,'$.sourceMessageId')
 FROM runs r JOIN tasks t ON t.id=r.task_id JOIN outbox o ON o.id=r.action_id
 WHERE t.bot_id=?1 AND r.environment_id=?3 AND r.thread_id=?4 AND json_extract(o.data,'$.tool')='delegate'
 UNION ALL
 SELECT t.id,json_extract(t.data,'$.sourceMessageId')
 FROM ancestry a JOIN peer_exchanges p ON p.recipient_task=a.id JOIN tasks t ON t.id=p.sender_task
 WHERE a.source IS NULL AND p.sender_task<a.id
)
SELECT m.id,m.bot_id,json_extract(m.routing,'$.sessionInvitationDepth'),
       json_extract(m.routing,'$.sessionOrigin.environmentId'),json_extract(m.routing,'$.sessionOrigin.threadId')
FROM ancestry a JOIN messages m ON m.id=a.source WHERE m.person_id=?2
)SQL",
                                    {message.bot, actor.id, target.environmentId, target.threadId});
                if (!launch.next()) throw Conflict("Invite a Bot into a Session you participate in or launched");
                const SessionTarget origin{launch.string(3), launch.string(4)};
                if (!origin.environmentId.empty() &&
                    !activeSessionOrigin(launch.string(1), actor.id, origin, target.environmentId, launch.number(0)))
                    throw Conflict(unavailable);
                depth = std::max(depth, launch.number(2));
            }
            if (depth >= maxDepth) throw Conflict("Invitations can reach a maximum depth of 3");

            auto invitee = participation(bot);
            if (invitee.next()) {
                if (invitee.string(1) != actor.id) throw Conflict("This Session participation is unavailable");
                if (invitee.string(2) == "removed") throw Conflict("The person removed this Bot from the Session");
            }
            const auto scrub = sharedRedactor();
            for (const auto id : std::array<std::string_view, 4>{bot, message.bot, target.environmentId, target.threadId}) {
                scrub.requirePublic(id);
                if (redactT3Tokens(std::string(id)) != id) throw Conflict("Invalid Session invitation identifier");
            }
            return {actor, inviterId, depth + 1};
        };
        {
            std::lock_guard lock(mutex);
            if (!replay().is_null()) return finish([] { return Json(); });
            authorize();
        }

        std::optional<SessionMetadata> metadata;
        try {
            metadata = t3.sessionMetadata(target, false, std::chrono::steady_clock::now() + std::chrono::seconds(5));
        } catch (const std::exception&) {
            throw Conflict("Could not verify access to this T3 Session");
        }
        if (!metadata || metadata->projectId.empty()) throw Conflict(unavailable);

        return finish([&] {
            const auto sender = authorize();
            const auto scrub = sharedRedactor();
            const auto safeNote = redactT3Tokens(scrub.text(note));
            const auto title = bounded(redactT3Tokens(scrub.text(metadata->title)), 512);
            const auto inviterName = redactT3Tokens(scrub.text(message.routing.at("config").value("botDisplayName", message.bot)));
            const auto description =
                "Invited to Session " + (title.empty() ? target.threadId : title) + " by " + inviterName + ", note: " + safeNote;
            Intake routing;
            try {
                if (!intake) throw Conflict("Bot intake is unavailable");
                routing = intake(bot, description);
            } catch (const StorageError&) {
                throw;
            } catch (const std::exception&) {
                throw Conflict("The invited Bot is unavailable");
            }
            if (routing.botId != bot) throw Conflict("The invited Bot is unavailable");

            const auto admit = [&](std::string_view id) {
                exec(R"SQL(
INSERT INTO session_participations(bot_id,environment_id,thread_id,person_id,state) VALUES(?,?,?,?,'active')
ON CONFLICT(bot_id,environment_id,thread_id) DO UPDATE SET state='active' WHERE state='left'
)SQL",
                     {id, target.environmentId, target.threadId, sender.actor.id});
                auto row = participation(id);
                if (!row.next()) throw Conflict("This Session participation is unavailable");
                return row.number(0);
            };
            const auto inviter = sender.participation ? sender.participation : admit(message.bot);
            const auto invitee = admit(bot);
            const auto key = "session-invite:" + std::to_string(message.id) + ":" + std::to_string(step);
            const SessionOrigin origin{target, title};
            const Json invitation{{"environmentId", target.environmentId},
                                  {"threadId", target.threadId},
                                  {"title", title},
                                  {"inviterBotId", message.bot},
                                  {"inviterName", inviterName},
                                  {"note", safeNote},
                                  {"depth", sender.depth}};
            const auto invited = enqueueConversation(sender.actor, routing, conversation::PrincipalSource::session, key, description, 0,
                                                     &origin, invitation);
            exec("INSERT INTO session_invitations VALUES(?,?,?,?,?,?)", {message.id, step, inviter, invitee, sender.depth, invited});
            return Json{{"ok", true}, {"messageId", invited}, {"depth", sender.depth}};
        });
    } catch (const Conflict& error) {
        return finish([&] { return toolFailure(error.what()); });
    } catch (const conversation_storage::ToolError& error) {
        return finish([&] { return toolFailure(error.what()); });
    }
}

Json SessionStopResult::data() const {
    return {{"stopped", true},
            {"cancelled", cancelled},
            {"alreadyStarted", alreadyStarted},
            {"notCancellable", notCancellable},
            {"pending", pending},
            {"stoppedChildSessions", stoppedChildSessions}};
}

std::string SessionStopResult::summary() const {
    auto text = "The Bot's participation in this Session has stopped. Flock cancelled " + std::to_string(cancelled) +
                (cancelled == 1 ? " queued message. " : " queued messages. ") + std::to_string(alreadyStarted) +
                (alreadyStarted == 1 ? " message had" : " messages had") +
                " already started and may still be processed. Flock requested cancellation for " + std::to_string(stoppedChildSessions) +
                (stoppedChildSessions == 1 ? " child Session." : " child Sessions.");
    if (notCancellable)
        text += " Flock could not check " + std::to_string(notCancellable) +
                (notCancellable == 1 ? " Run's queue status." : " Runs' queue status.");
    if (pending) text += " Flock will retry cancellation for " + std::to_string(pending) + (pending == 1 ? " message." : " messages.");
    return text;
}

SessionStopResult SessionParticipation::stop(const T3Connection& connection, std::string_view bot, std::string_view thread) {
    if (!validBotId(bot) || !validId(thread, 256)) throw Conflict("Invalid flock_stop arguments");

    auto& db = *store_.p_;
    SessionStop stopped;
    {
        const SessionDispatchFileLock dispatch(db.path, SessionDispatchFileLock::Mode::exclusive);
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        stopped = db.stopSessionParticipation(person(connection), bot, {"", std::string(thread)}, SessionExit::removed);
        transaction.commit();
    }
    return db.settleSessionQueueCancellations(t3_, {}, stopped);
}

// Both entry points hold the exclusive dispatch lock and a write transaction here.
SessionStop Store::Impl::stopSessionParticipation(const Person& actor, std::string_view bot, const SessionTarget& target, SessionExit exit,
                                                  Id message) {
    Policy policy(*this);
    if (!policy.allows(actor, Action::bot_decide, bot)) throw Conflict("Bot access is required");
    const bool throughConnector =
        exit == SessionExit::left && actor.adminRoot && message && connectedSessionAccess(loadTurnMessage(message), target);
    auto row = query(R"SQL(
SELECT p.id,p.environment_id,p.state,p.stopped_through FROM session_participations p
JOIN t3_connector c ON c.person_id=p.person_id
JOIN t3_connected_environments e ON e.environment_id=p.environment_id
WHERE (? OR p.person_id=?) AND p.bot_id=? AND p.thread_id=? AND (?='' OR p.environment_id=?)
)SQL",
                     {throughConnector ? 1 : 0, actor.id, bot, target.threadId, target.environmentId, target.environmentId});
    if (!row.next()) throw Conflict("This Session participation is unavailable");
    const auto id = row.number(0);
    const auto environment = row.string(1);
    const auto current = row.string(2);
    const auto stoppedThrough = row.number(3);
    if (row.next()) throw Conflict("This threadId matched more than one connected T3 environment");
    if (!policy.allows(bot, Action::environment_inspect, environment))
        throw Conflict("This Session is unavailable through your T3 connection");
    // A retried leave from an earlier turn must not stop a newly reactivated participation.
    if ((message && message <= stoppedThrough) || current == "removed" || (current == "left" && exit == SessionExit::left))
        return {id, stoppedThrough};

    // A send may have returned a receipt before its transaction hit SQLite contention.
    // Include that Run in the same stop transaction even if the conversation step is retried later.
    const SessionTarget stoppedTarget{environment, target.threadId};
    for (const auto& [source, receipt] : sessionSendReceipts)
        if (receipt.target == stoppedTarget) recordSessionSendRun(source, receipt);

    // The journal ID identifies this stop and fences out all previously admitted turns.
    const auto state = exit == SessionExit::removed ? "removed" : "left";
    exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'session_participation_stop',?,?)",
         {policyClock(), Json{{"participationId", id}, {"state", state}}, bot});
    const auto cutoff = last();
    exec((stopCascade + R"SQL(
UPDATE session_participations SET state=?3, stopped_through=?2
WHERE id IN stopping AND state<>'removed'
)SQL")
             .c_str(),
         {id, cutoff, state});
    exec((stopCascade + R"SQL(
INSERT OR IGNORE INTO session_queue_cancellations(participation_id,run,stop_id)
SELECT p.id,w.run,?2 FROM stopping s JOIN session_participations p ON p.id=s.id
JOIN session_message_waits w ON w.bot=p.bot_id AND w.environment=p.environment_id AND w.thread=p.thread_id
UNION ALL
SELECT s.id,r.run_id,?2 FROM stopping s JOIN outbox o ON json_extract(o.data,'$.sessionParticipationId')=s.id
JOIN runs r ON r.action_id=o.id
)SQL")
             .c_str(),
         {id, cutoff});
    exec((stopCascade + R"SQL(
UPDATE session_message_waits SET notified=1 WHERE notified=0 AND rowid IN (
 SELECT w.rowid FROM stopping s JOIN session_participations p ON p.id=s.id
 JOIN session_message_waits w ON w.bot=p.bot_id
 JOIN messages m ON m.id=w.message_id
 WHERE (w.environment=p.environment_id AND w.thread=p.thread_id)
    OR (json_extract(m.routing,'$.sessionOrigin.environmentId')=p.environment_id
        AND json_extract(m.routing,'$.sessionOrigin.threadId')=p.thread_id AND m.person_id=p.person_id)
)
)SQL")
             .c_str(),
         {id, cutoff});
    auto turns = query((stopCascade + R"SQL(
SELECT DISTINCT m.id FROM stopping s JOIN session_participations p ON p.id=s.id
JOIN messages m ON m.bot_id=p.bot_id JOIN conversation_turns t ON t.message_id=m.id
WHERE t.status IN ('paused','policy_waiting') AND (
 (json_extract(m.routing,'$.sessionOrigin.environmentId')=p.environment_id
  AND json_extract(m.routing,'$.sessionOrigin.threadId')=p.thread_id AND m.person_id=p.person_id)
 OR (json_extract(m.routing,'$.sessionWake.environmentId')=p.environment_id
     AND json_extract(m.routing,'$.sessionWake.threadId')=p.thread_id)
)
)SQL")
                           .c_str(),
                       {id, cutoff});
    std::vector<Id> closedTurns;
    while (turns.next()) closedTurns.push_back(turns.number(0));
    for (const auto turn : closedTurns)
        finishTurn(loadTurnMessage(turn), "The T3 Session participation has ended.", nullptr, "session_access");
    // Persist each cancellation intent with the stop. Runtime recovers in-flight launches and
    // interrupts their own assigned Runs through the ordinary task cancellation path.
    auto children = query((stopCascade + R"SQL(
SELECT t.id,p.id FROM stopping s JOIN session_participations p ON p.id=s.id
JOIN tasks t ON t.bot_id=p.bot_id
JOIN messages m ON m.id=json_extract(t.data,'$.sourceMessageId') AND m.bot_id=p.bot_id AND m.person_id=p.person_id
WHERE json_extract(t.data,'$.sessionOrigin.environmentId')=p.environment_id
  AND json_extract(t.data,'$.sessionOrigin.threadId')=p.thread_id
  AND t.state NOT IN ('completed','cancelled','cancelling')
  AND json_type(t.data,'$.selectedSession') IS NULL
  AND NOT (json_extract(t.data,'$.activeRun.threadId') IS p.thread_id AND t.environment_id=p.environment_id)
ORDER BY t.id
)SQL")
                              .c_str(),
                          {id, cutoff});
    std::vector<std::pair<Id, Id>> cancellations;
    while (children.next()) cancellations.emplace_back(children.number(0), children.number(1));
    for (const auto& [taskId, participation] : cancellations) {
        auto task = load(taskId);
        if (!terminal(task) && task.at("state") != "cancelling") requestCancellation(task, {participation, cutoff});
    }
    auto selected = query((stopCascade + R"SQL(
SELECT t.id,m.id,p.person_id,p.environment_id,p.thread_id
FROM stopping s JOIN session_participations p ON p.id=s.id
JOIN tasks t ON t.bot_id=p.bot_id
JOIN messages m ON m.id=json_extract(t.data,'$.sourceMessageId')
LEFT JOIN outbox o ON o.id=json_extract(t.data,'$.actionId')
WHERE json_extract(t.data,'$.selectedSession.environmentId')=p.environment_id
  AND json_extract(t.data,'$.selectedSession.threadId')=p.thread_id
  AND t.state NOT IN ('completed','cancelled','cancelling')
  AND json_type(t.data,'$.activeRun')='null' AND o.phase IS NOT 'started'
)SQL")
                              .c_str(),
                          {id, cutoff});
    std::vector<Id> unassigned;
    while (selected.next()) {
        const auto source = loadTurnMessage(selected.number(1));
        if (source.admittedActor.id == selected.string(2) || connectedSessionAccess(source, {selected.string(3), selected.string(4)}))
            unassigned.push_back(selected.number(0));
    }
    for (const auto taskId : unassigned) {
        auto task = load(taskId);
        requestCancellation(task);
    }
    auto approvals = query((stopCascade + R"SQL(
UPDATE policy_approvals SET status='stale' WHERE status IN ('pending','approved') AND turn<>0
 AND action IN ('t3.send','session.read') AND EXISTS (
 SELECT 1 FROM stopping s JOIN session_participations p ON p.id=s.id
 WHERE bot=p.bot_id AND target=json_array(p.environment_id,p.thread_id)
 ) RETURNING turn
)SQL")
                               .c_str(),
                           {id, cutoff});
    std::vector<Id> released;
    while (approvals.next()) released.push_back(approvals.number(0));
    for (const auto turn : released) releasePolicyTurn(turn);
    settlePolicyApprovals();
    return {id, cutoff};
}

SessionStopResult Store::Impl::settleSessionQueueCancellations(T3& t3, std::stop_token shutdown, SessionStop stop) {
    // Serialize immediate tool results and background retries. Never hold the database
    // or dispatch lock across T3 cancellation calls.
    std::lock_guard cancellation(sessionQueueCancellationMutex);
    struct Pending {
        Id participant;
        SessionTarget target;
        std::string run;
    };
    std::vector<Pending> pending;
    {
        std::lock_guard lock(mutex);
        auto rows = query((stopCascade + R"SQL(
SELECT c.participation_id,p.environment_id,p.thread_id,c.run
FROM session_queue_cancellations c JOIN session_participations p ON p.id=c.participation_id
JOIN t3_connector t ON t.person_id=p.person_id
JOIN t3_connected_environments e ON e.environment_id=p.environment_id
WHERE c.state='pending' AND c.retry_at<=?3 AND (?1=0 OR (c.stop_id=?2 AND c.participation_id IN stopping))
ORDER BY c.participation_id,c.run
)SQL")
                              .c_str(),
                          {stop.participation, stop.cutoff, policyClock()});
        while (rows.next()) pending.push_back({rows.number(0), {rows.string(1), rows.string(2)}, rows.string(3)});
    }
    for (const auto& item : pending) {
        if (shutdown.stop_requested()) break;
        auto outcome = QueueCancelOutcome::Retry;
        try {
            outcome = t3.cancelQueuedSessionMessage(item.target, item.run);
        } catch (const std::exception&) {
            // A lost reply can be retried for this exact Run.
        }
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        switch (outcome) {
        case QueueCancelOutcome::Retry:
            exec("UPDATE session_queue_cancellations SET retry_at=? WHERE participation_id=? AND run=?",
                 {policyClock() + 5, item.participant, item.run});
            break;
        case QueueCancelOutcome::Cancelled:
        case QueueCancelOutcome::AlreadyStarted:
        case QueueCancelOutcome::NotCancellable:
            const auto state = outcome == QueueCancelOutcome::Cancelled        ? "cancelled"
                               : outcome == QueueCancelOutcome::AlreadyStarted ? "started"
                                                                               : "unavailable";
            exec("UPDATE session_queue_cancellations SET state=? WHERE participation_id=? AND run=?", {state, item.participant, item.run});
            if (outcome == QueueCancelOutcome::Cancelled) {
                exec(R"SQL(
UPDATE session_message_waits SET notified=1,run_finished=1,interrupt_at=NULL
WHERE bot=(SELECT bot_id FROM session_participations WHERE id=?) AND environment=? AND thread=? AND run=?
)SQL",
                     {item.participant, item.target.environmentId, item.target.threadId, item.run});
                auto run = query(R"SQL(
SELECT r.task_id FROM runs r JOIN outbox o ON o.id=r.action_id
WHERE r.environment_id=? AND r.thread_id=? AND r.run_id=?
 AND json_extract(o.data,'$.sessionParticipationId')=?
)SQL",
                                 {item.target.environmentId, item.target.threadId, item.run, item.participant});
                if (run.next()) {
                    auto task = load(run.number(0));
                    const auto& active = task.at("activeRun");
                    if (!terminal(task) && active.is_object() && field(active, "runId") == item.run &&
                        field(active, "threadId") == item.target.threadId && field(active, "environmentId") == item.target.environmentId) {
                        // Queue cancellation confirms this Run never started.
                        task["activeRun"] = nullptr;
                        if (task.at("state") == "cancelling")
                            save(task);
                        else
                            requestCancellation(task);
                    }
                }
            }
            break;
        }
        transaction.commit();
    }
    if (!stop.participation) {
        interruptPausedSessionRuns(t3, shutdown);
        return {};
    }
    SessionStopResult result;
    std::lock_guard lock(mutex);
    auto rows = query((stopCascade + R"SQL(
SELECT state,count(*) FROM session_queue_cancellations WHERE stop_id=?2 AND participation_id IN stopping GROUP BY state
)SQL")
                          .c_str(),
                      {stop.participation, stop.cutoff});
    while (rows.next()) {
        const auto state = rows.string(0);
        if (state == "cancelled")
            result.cancelled = rows.number(1);
        else if (state == "started")
            result.alreadyStarted = rows.number(1);
        else if (state == "unavailable")
            result.notCancellable = rows.number(1);
        else
            result.pending = rows.number(1);
    }
    auto children = query((stopCascade + R"SQL(
SELECT count(*) FROM journal WHERE kind='cancel_requested'
 AND json_extract(data,'$.sessionStopId')=?2 AND json_extract(data,'$.sessionParticipationId') IN stopping
)SQL")
                              .c_str(),
                          {stop.participation, stop.cutoff});
    if (children.next()) result.stoppedChildSessions = children.number(0);
    return result;
}

bool Store::Impl::leaveSession(const conversation::Message& message, int step, const Decision& decision, T3& t3, std::stop_token shutdown) {
    SessionStop stop;
    {
        const SessionDispatchFileLock dispatch(path, SessionDispatchFileLock::Mode::exclusive);
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested()) return false;
        if (!authorizeTurn(message)) {
            transaction.commit();
            return false;
        }
        if (!query("SELECT 1 FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL", {message.id, step}).next())
            return false;
        try {
            using conversation_storage::text;
            const auto& args = decision.args;
            if (!args.is_object() || args.size() != 2) throw Conflict("Invalid leave_session arguments");
            const SessionTarget target{text(args, "environmentId", 256, true), text(args, "threadId", 256, true)};
            if (!validId(target.environmentId, 256) || !validId(target.threadId, 256)) throw Conflict("Invalid leave_session arguments");
            stop = stopSessionParticipation(turnActor(message), message.bot, target, SessionExit::left, message.id);
        } catch (const Conflict& error) {
            saveTurnResults(message.id, step, Json::array({toolFailure(error.what())}));
        } catch (const conversation_storage::ToolError& error) {
            saveTurnResults(message.id, step, Json::array({toolFailure(error.what())}));
        }
        transaction.commit();
    }
    if (!stop.participation) return true;
    const auto stopped = settleSessionQueueCancellations(t3, shutdown, stop);
    std::lock_guard lock(mutex);
    Transaction transaction(db);
    auto result = stopped.data();
    result["ok"] = true;
    saveTurnResults(message.id, step, Json::array({result}));
    // A Session-origin turn cannot take another model step once its own origin stops.
    if (message.principalSource == conversation::PrincipalSource::session && !activeSessionOrigin(message) && isNextTurn(message.id)) {
        finishTurn(message, stopped.summary(), nullptr, "session_left");
        transaction.commit();
        return false;
    }
    transaction.commit();
    return true;
}

bool Store::Impl::activeSessionOrigin(const conversation::Message& message) const {
    const auto& origin = message.routing.at("sessionOrigin");
    const SessionTarget target{origin.at("environmentId"), origin.at("threadId")};
    return activeSessionOrigin(message.bot, message.admittedActor.id, target, target.environmentId, message.id);
}

bool Store::Impl::activeSessionOrigin(std::string_view bot, std::string_view person, const SessionTarget& origin,
                                      std::string_view destinationEnvironment, Id message) const {
    return query(R"SQL(
SELECT 1 FROM session_participations p
JOIN t3_connected_environments e ON e.environment_id=p.environment_id
JOIN t3_connector c ON c.person_id=p.person_id
JOIN t3_connected_environments destination ON destination.environment_id=?
WHERE p.bot_id=? AND p.environment_id=? AND p.thread_id=? AND p.person_id=? AND p.state='active'
  AND p.stopped_through<?
)SQL",
                 {destinationEnvironment, bot, origin.environmentId, origin.threadId, person, message})
        .next();
}

bool Store::Impl::sessionParticipationStopped(std::string_view bot, const SessionTarget& target, Id message) const {
    return query(R"SQL(
SELECT 1 FROM session_participations WHERE bot_id=? AND environment_id=? AND thread_id=?
  AND (state<>'active' OR (?<>0 AND stopped_through>=?))
)SQL",
                 {bot, target.environmentId, target.threadId, message, message})
        .next();
}

}
