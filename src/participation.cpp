#include "flock/participation.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <array>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace flockv0 {
namespace {

constexpr std::string_view tokenPrefix = "flock-t3-";
constexpr std::size_t maxRequestBytes = 8192;

template<class Bytes>
std::string hex(const Bytes& bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

std::string digest(std::string_view value) {
    std::array<unsigned char,32> bytes{};
    unsigned int size = 0;
    if (EVP_Digest(value.data(),value.size(),bytes.data(),&size,EVP_sha256(),nullptr) != 1 || size != bytes.size())
        throw std::runtime_error("Could not hash T3 connection data");
    return hex(bytes);
}

bool validId(std::string_view value, std::size_t max = 1024) {
    return !value.empty() && value.size() <= max &&
           std::ranges::none_of(value,[](unsigned char c) { return c <= 32 || c == 127; });
}

}

std::string redactT3Tokens(std::string value) {
    std::size_t at = 0;
    while ((at = value.find(tokenPrefix,at)) != value.npos) {
        const auto suffix = std::string_view(value).substr(at + tokenPrefix.size(),64);
        if (suffix.size() == 64 && suffix.find_first_not_of("0123456789abcdef") == suffix.npos)
            value.replace(at,tokenPrefix.size()+64,"[T3 connection token]");
        at += tokenPrefix.size();
    }
    return value;
}

void Store::Impl::initializeParticipationSchema() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS session_participations(
 id INTEGER PRIMARY KEY,
 bot_id TEXT NOT NULL, environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, person_id TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('active','stopped')),
 UNIQUE(bot_id,environment_id,thread_id))
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS session_asks(
 person_id TEXT NOT NULL, request_id TEXT NOT NULL,
 participation_id INTEGER NOT NULL REFERENCES session_participations(id),
 request_digest TEXT NOT NULL, message_id INTEGER NOT NULL UNIQUE REFERENCES messages(id),
 PRIMARY KEY(person_id,request_id))
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS session_invitations(
 source_message_id INTEGER NOT NULL, step INTEGER NOT NULL,
 inviter_participation_id INTEGER NOT NULL REFERENCES session_participations(id),
 participation_id INTEGER NOT NULL REFERENCES session_participations(id),
 depth INTEGER NOT NULL CHECK(depth BETWEEN 1 AND 3),
 message_id INTEGER NOT NULL UNIQUE REFERENCES messages(id),
 PRIMARY KEY(source_message_id,step),
 FOREIGN KEY(source_message_id,step) REFERENCES conversation_steps(message_id,step))
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS t3_connector(
 singleton INTEGER PRIMARY KEY CHECK(singleton=1), id TEXT NOT NULL, person_id TEXT NOT NULL,
 token_digest TEXT NOT NULL, port INTEGER NOT NULL)
)SQL");
    exec("CREATE TABLE IF NOT EXISTS t3_connected_environments(environment_id TEXT PRIMARY KEY)");
}

std::string T3Connections::connect(std::string_view environment, std::string_view person,
                                  unsigned port,
                                  const std::function<void(std::string_view)>& install, bool refreshProviders) {
    if (!validId(environment) || !validId(person) || port == 0 || port > 65535)
        throw Conflict("Invalid T3 connection");
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto rotate = refreshProviders || [&] {
        auto row = db.query("SELECT person_id,port FROM t3_connector");
        return !row.next() || row.string(0) != person || row.number(1) != port ||
            !db.query("SELECT 1 FROM t3_connected_environments LIMIT 1").next() ||
            db.query("SELECT 1 FROM t3_connected_environments WHERE environment_id=?",{environment}).next();
    }();
    std::string token;
    if (rotate) {
        std::array<unsigned char,32> bytes{};
        if (RAND_bytes(bytes.data(),static_cast<int>(bytes.size())) != 1)
            throw std::runtime_error("Could not generate T3 connection token");
        token = std::string(tokenPrefix) + hex(bytes);
        db.exec(R"SQL(
INSERT INTO t3_connector VALUES(1,?,?,?,?)
ON CONFLICT(singleton) DO UPDATE SET id=excluded.id,person_id=excluded.person_id,
 token_digest=excluded.token_digest,port=excluded.port
)SQL",{actionId(),person,digest(token),static_cast<Id>(port)});
    }
    db.exec("INSERT OR IGNORE INTO t3_connected_environments VALUES(?)",{environment});
    if (install) install(token);
    tx.commit();
    return token;
}

bool T3Connections::ownsBearer(std::string_view token) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    return db.query("SELECT 1 FROM t3_connector WHERE token_digest=?",{digest(token)}).next();
}

bool T3Connections::disconnect(std::string_view environment) {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    db.exec("DELETE FROM t3_connected_environments WHERE environment_id=?",{environment});
    const bool removeProvider = db.query("SELECT 1 FROM t3_connector WHERE "
                                        "NOT EXISTS(SELECT 1 FROM t3_connected_environments)").next();
    tx.commit();
    return removeProvider;
}

void T3Connections::providerRemoved() {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    db.exec("DELETE FROM t3_connector WHERE NOT EXISTS(SELECT 1 FROM t3_connected_environments)");
}

std::optional<T3Connection> T3Connections::authenticate(std::string_view token) const {
    if (!token.starts_with(tokenPrefix) || token.size() != tokenPrefix.size()+64 ||
        token.substr(tokenPrefix.size()).find_first_not_of("0123456789abcdef") != token.npos) return {};
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    auto row = db.query("SELECT id,person_id FROM t3_connector WHERE token_digest=?"
                        " AND EXISTS(SELECT 1 FROM t3_connected_environments)",{digest(token)});
    if (!row.next()) return {};
    return T3Connection{row.string(0),row.string(1)};
}

std::optional<T3ConnectionSettings> T3Connections::settings(std::string_view person, bool installationRoot) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    auto rows = db.query("SELECT port,environment_id FROM t3_connector CROSS JOIN t3_connected_environments"
                         " WHERE (? OR person_id=?) ORDER BY environment_id",{installationRoot ? 1 : 0,person});
    if (!rows.next()) return {};
    T3ConnectionSettings result{static_cast<unsigned>(rows.number(0)),{}};
    do { result.environments.push_back(rows.string(1)); } while (rows.next());
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

Person Store::Impl::sessionActor(std::string_view person) {
    if (resolveWebIdentity) {
        if (const auto root = resolveWebIdentity(person)) return {std::string(person),*root};
    } else if (person == localOwner.id && !Policy(*this).requiresIdentity()) return localOwner;
    return {};
}

Person SessionParticipation::person(const T3Connection& connection) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    if (!db.query("SELECT 1 FROM t3_connector WHERE id=? AND person_id=? "
                  "AND EXISTS(SELECT 1 FROM t3_connected_environments)",{connection.id,connection.personId}).next())
        throw Conflict("The T3 connection is unavailable");
    const auto actor = db.sessionActor(connection.personId);
    if (actor.id.empty()) throw Conflict("The connected person is unavailable");
    return actor;
}

Id SessionParticipation::ask(const T3Connection& connection, const SessionAsk& request, const IntakeResolver& intake) {
    const auto& [bot,text,thread,requestId] = request;
    if (!validBotId(bot) || !validId(thread,256) || text.size() > maxRequestBytes ||
        text.find('\0') != text.npos || !conversation_storage::hasText(text) ||
        requestId.empty() || requestId.size() > 128 ||
        requestId.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-") != requestId.npos)
        throw Conflict("Invalid flock_ask arguments");

    auto& db = *store_.p_;
    const auto policy = store_.policy();
    const auto authorizedPerson = [&] {
        const auto actor = person(connection);
        if (!policy.allows(actor,Action::bot_decide,bot)) throw Conflict("Bot access is required");
        return actor;
    };
    const auto fingerprint = digest(Json::array({bot,text,thread}).dump());
    const auto connected = [&](std::string_view environment) {
        return db.query("SELECT 1 FROM t3_connected_environments WHERE environment_id=?",{environment}).next() &&
            policy.allows(bot,Action::environment_inspect,environment);
    };
    const auto replay = [&]() -> Id {
        auto row = db.query(R"SQL(
SELECT a.message_id,a.request_digest,p.environment_id FROM session_asks a
JOIN session_participations p ON p.id=a.participation_id WHERE a.person_id=? AND a.request_id=?
)SQL",{connection.personId,requestId});
        if (!row.next()) return 0;
        if (row.string(1) != fingerprint) throw Conflict("This requestId already has different arguments");
        if (!connected(row.string(2))) throw Conflict("This Session is unavailable through your T3 connection");
        return row.number(0);
    };

    std::vector<std::string> environments;
    {
        std::lock_guard lock(db.mutex);
        authorizedPerson();
        if (const auto message = replay()) return message;
        const auto settings = T3Connections(store_).settings(connection.personId);
        if (settings)
            for (const auto& environment : settings->environments)
                if (connected(environment)) environments.push_back(environment);
    }
    std::optional<SessionOrigin> origin;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (const auto& environment : environments) {
        SessionTarget candidate{environment,thread};
        std::optional<SessionMetadata> metadata;
        try {
            metadata = t3_.sessionMetadata(candidate,false,deadline);
        } catch (const std::exception&) {
            continue;
        }
        if (!metadata || metadata->projectId.empty()) continue;
        if (origin) throw Conflict("This threadId matched more than one connected T3 environment");
        origin = SessionOrigin{std::move(candidate),std::move(metadata->title)};
    }
    std::lock_guard lock(db.mutex);
    Transaction transaction(db.db);
    const auto actor = authorizedPerson();
    if (const auto message = replay()) return message;
    if (!origin || !connected(origin->target.environmentId))
        throw Conflict("This Session is unavailable through your T3 connection");
    const auto scrub = db.sharedRedactor();
    for (const std::string_view id : {std::string_view(bot),std::string_view(origin->target.environmentId),
                                     std::string_view(thread),std::string_view(requestId)}) {
        scrub.requirePublic(id);
        if (redactT3Tokens(std::string(id)) != id) throw Conflict("Invalid Session ask identifier");
    }
    origin->title = redactT3Tokens(scrub.text(origin->title));
    db.exec(R"SQL(
INSERT INTO session_participations(bot_id,environment_id,thread_id,person_id,state) VALUES(?,?,?,?,'active')
ON CONFLICT(bot_id,environment_id,thread_id) DO NOTHING
)SQL",{bot,origin->target.environmentId,thread,actor.id});
    auto row = db.query("SELECT id,person_id,state FROM session_participations WHERE bot_id=? AND environment_id=? AND thread_id=?",
                        {bot,origin->target.environmentId,thread});
    if (!row.next() || row.string(1) != actor.id) throw Conflict("This Session participation is unavailable");
    if (row.string(2) == "stopped") throw Conflict("This Bot's participation in the Session has stopped");
    const auto safeText = redactT3Tokens(scrub.text(text));
    const auto routing = intake(bot,safeText);
    if (routing.botId != bot) throw Conflict("The Bot is unavailable");
    const auto message = db.enqueueConversation(actor,routing,conversation::PrincipalSource::session,
                                                 "session-ask:" + requestId,safeText,0,&*origin);
    db.exec("INSERT INTO session_asks VALUES(?,?,?,?,?)",{actor.id,requestId,row.number(0),fingerprint,message});
    transaction.commit();
    return message;
}

bool Store::Impl::inviteSessionBot(const conversation::Message& message, int step, const Decision& decision,
                                  T3& t3, const IntakeResolver& intake, std::stop_token shutdown) {
    constexpr Id maxDepth = 3;
    constexpr auto stopped = "This Bot's participation in the Session has stopped";
    constexpr auto unavailable = "This Session is unavailable through your T3 connection";
    const auto replay = [&]() -> Json {
        auto row = query("SELECT message_id,depth FROM session_invitations WHERE source_message_id=? AND step=?",
                         {message.id,step});
        if (!row.next()) return nullptr;
        return {{"ok",true},{"messageId",row.number(0)},{"depth",row.number(1)}};
    };
    const auto finish = [&](const auto& apply) {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested()) return false;
        if (!authorizeTurn(message)) { transaction.commit(); return false; }
        if (query("SELECT 1 FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",
                  {message.id,step}).next()) {
            auto result = replay();
            if (result.is_null()) result = apply();
            saveTurnResults(message.id,step,Json::array({result}));
        }
        transaction.commit();
        return true;
    };

    try {
        const auto& args = decision.args;
        using conversation_storage::text;
        if (!args.is_object() || args.size() != 4) throw Conflict("Invalid invite_bot arguments");
        const auto bot = text(args,"botId",128,true);
        const SessionTarget target{text(args,"environmentId",256,true),text(args,"threadId",256,true)};
        const auto note = text(args,"note",maxRequestBytes,true);
        if (!validBotId(bot) || !validId(target.environmentId,256) || !validId(target.threadId,256) ||
            note.find('\0') != note.npos)
            throw Conflict("Invalid invite_bot arguments");
        if (bot == message.bot) throw Conflict("A Bot cannot invite itself");

        struct Sender { Person actor; Id participation, depth; };
        const auto participation = [&](std::string_view id) {
            return query("SELECT id,person_id,state FROM session_participations WHERE bot_id=? AND environment_id=? AND thread_id=?",
                         {id,target.environmentId,target.threadId});
        };
        const auto authorize = [&]() -> Sender {
            const auto actor = turnActor(message);
            Policy policy(*this);
            if (!policy.allows(actor,Action::bot_decide,message.bot) || !policy.allows(actor,Action::bot_decide,bot))
                throw Conflict("Bot access is required");
            if (!policy.allows(message.bot,Action::environment_inspect,target.environmentId) ||
                !policy.allows(bot,Action::environment_inspect,target.environmentId) ||
                !query("SELECT 1 FROM t3_connector CROSS JOIN t3_connected_environments "
                       "WHERE person_id=? AND environment_id=?",{actor.id,target.environmentId}).next())
                throw Conflict(unavailable);
            if (message.routing.contains("sessionOrigin") && !activeSessionOrigin(message)) throw Conflict(stopped);

            auto inviter = participation(message.bot);
            auto depth = message.routing.value("sessionInvitationDepth",Id{0});
            Id inviterId = 0;
            if (inviter.next()) {
                if (inviter.string(2) == "stopped") throw Conflict(stopped);
                if (inviter.string(1) != actor.id) throw Conflict(unavailable);
                inviterId = inviter.number(0);
            } else {
                auto launch = query(R"SQL(
SELECT json_extract(m.routing,'$.sessionInvitationDepth'),
       json_extract(t.data,'$.sessionOrigin.environmentId'),json_extract(t.data,'$.sessionOrigin.threadId')
FROM runs r JOIN tasks t ON t.id=r.task_id JOIN outbox o ON o.id=r.action_id
JOIN messages m ON m.id=json_extract(t.data,'$.sourceMessageId') AND m.bot_id=t.bot_id
WHERE t.bot_id=? AND m.person_id=? AND r.environment_id=? AND r.thread_id=?
  AND json_extract(o.data,'$.tool')='delegate'
)SQL",{message.bot,actor.id,target.environmentId,target.threadId});
                if (!launch.next()) throw Conflict("Invite a Bot into a Session you participate in or launched");
                const SessionTarget origin{launch.string(1),launch.string(2)};
                if (!origin.environmentId.empty() && !activeSessionOrigin(message.bot,actor.id,origin,target.environmentId))
                    throw Conflict(unavailable);
                depth = std::max(depth,launch.number(0));
            }
            if (depth >= maxDepth) throw Conflict("Invitations can reach a maximum depth of 3");

            auto invitee = participation(bot);
            if (invitee.next()) {
                if (invitee.string(1) != actor.id) throw Conflict("This Session participation is unavailable");
                if (invitee.string(2) == "stopped") throw Conflict("The invited Bot's participation in the Session has stopped");
            }
            const auto scrub = sharedRedactor();
            for (const auto id : std::array<std::string_view,4>{bot,message.bot,target.environmentId,target.threadId}) {
                scrub.requirePublic(id);
                if (redactT3Tokens(std::string(id)) != id) throw Conflict("Invalid Session invitation identifier");
            }
            return {actor,inviterId,depth + 1};
        };
        {
            std::lock_guard lock(mutex);
            if (!replay().is_null()) return finish([] { return Json(); });
            authorize();
        }

        std::optional<SessionMetadata> metadata;
        try {
            metadata = t3.sessionMetadata(target,false,std::chrono::steady_clock::now() + std::chrono::seconds(5));
        } catch (const std::exception&) {
            throw Conflict("Could not verify access to this T3 Session");
        }
        if (!metadata || metadata->projectId.empty()) throw Conflict(unavailable);

        return finish([&] {
            const auto sender = authorize();
            const auto scrub = sharedRedactor();
            const auto safeNote = redactT3Tokens(scrub.text(note));
            const auto title = bounded(redactT3Tokens(scrub.text(metadata->title)),512);
            const auto inviterName = redactT3Tokens(scrub.text(message.routing.at("config").value("botDisplayName",message.bot)));
            const auto description = "Invited to Session " + (title.empty() ? target.threadId : title) +
                " by " + inviterName + ", note: " + safeNote;
            Intake routing;
            try {
                if (!intake) throw Conflict("Bot intake is unavailable");
                routing = intake(bot,description);
            } catch (const StorageError&) {
                throw;
            } catch (const std::exception&) {
                throw Conflict("The invited Bot is unavailable");
            }
            if (routing.botId != bot) throw Conflict("The invited Bot is unavailable");

            const auto admit = [&](std::string_view id) {
                exec(R"SQL(
INSERT INTO session_participations(bot_id,environment_id,thread_id,person_id,state) VALUES(?,?,?,?,'active')
ON CONFLICT(bot_id,environment_id,thread_id) DO NOTHING
)SQL",{id,target.environmentId,target.threadId,sender.actor.id});
                auto row = participation(id);
                if (!row.next()) throw Conflict("This Session participation is unavailable");
                return row.number(0);
            };
            const auto inviter = sender.participation ? sender.participation : admit(message.bot);
            const auto invitee = admit(bot);
            const auto key = "session-invite:" + std::to_string(message.id) + ":" + std::to_string(step);
            const SessionOrigin origin{target,title};
            const Json invitation{{"environmentId",target.environmentId},{"threadId",target.threadId},
                                  {"inviterBotId",message.bot},{"inviterName",inviterName},
                                  {"note",safeNote},{"depth",sender.depth}};
            const auto invited = enqueueConversation(sender.actor,routing,conversation::PrincipalSource::session,
                                                      key,description,0,&origin,invitation);
            exec("INSERT INTO session_invitations VALUES(?,?,?,?,?,?)",
                 {message.id,step,inviter,invitee,sender.depth,invited});
            return Json{{"ok",true},{"messageId",invited},{"depth",sender.depth}};
        });
    } catch (const Conflict& error) {
        return finish([&] { return toolFailure(error.what()); });
    } catch (const conversation_storage::ToolError& error) {
        return finish([&] { return toolFailure(error.what()); });
    }
}

void SessionParticipation::stop(const T3Connection& connection, std::string_view bot, std::string_view thread) {
    if (!validBotId(bot) || !validId(thread,256)) throw Conflict("Invalid flock_stop arguments");

    auto& db = *store_.p_;
    // Wait for dispatches already admitted before committing the stop.
    std::lock_guard dispatch(db.sessionDispatchMutex);
    std::lock_guard lock(db.mutex);
    Transaction transaction(db.db);
    const auto actor = person(connection);
    if (!store_.policy().allows(actor,Action::bot_decide,bot)) throw Conflict("Bot access is required");

    auto row = db.query(R"SQL(
SELECT p.id,p.environment_id FROM session_participations p
JOIN t3_connected_environments e ON e.environment_id=p.environment_id
WHERE p.bot_id=? AND p.thread_id=? AND p.person_id=?
)SQL",{bot,thread,actor.id});
    if (!row.next()) throw Conflict("This Session participation is unavailable");
    const auto id = row.number(0);
    const auto environment = row.string(1);
    if (row.next()) throw Conflict("This threadId matched more than one connected T3 environment");

    db.exec("UPDATE session_participations SET state='stopped' WHERE id=?",{id});
    db.exec("UPDATE session_message_waits SET notified=1 WHERE notified=0 AND bot=? AND environment=? AND thread=?",
            {bot,environment,thread});
    transaction.commit();
}

bool Store::Impl::activeSessionOrigin(const conversation::Message& message) const {
    const auto& origin = message.routing.at("sessionOrigin");
    const SessionTarget target{origin.at("environmentId"),origin.at("threadId")};
    return activeSessionOrigin(message.bot,message.admittedActor.id,target,target.environmentId);
}

bool Store::Impl::activeSessionOrigin(std::string_view bot, std::string_view person, const SessionTarget& origin,
                                      std::string_view destinationEnvironment) const {
    return query(R"SQL(
SELECT 1 FROM session_participations p
JOIN t3_connected_environments e ON e.environment_id=p.environment_id
JOIN t3_connector c ON c.person_id=p.person_id
JOIN t3_connected_environments destination ON destination.environment_id=?
WHERE p.bot_id=? AND p.environment_id=? AND p.thread_id=? AND p.person_id=? AND p.state='active'
)SQL",{destinationEnvironment,bot,origin.environmentId,origin.threadId,person}).next();
}

bool Store::Impl::sessionParticipationStopped(std::string_view bot, const SessionTarget& target) const {
    return query("SELECT 1 FROM session_participations WHERE bot_id=? AND environment_id=? AND thread_id=? AND state='stopped'",
                 {bot,target.environmentId,target.threadId}).next();
}

}
