#include "flock/session_handoff.hpp"
#include "storage.hpp"

#include <algorithm>
#include <array>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace flockv0 {
namespace {

constexpr std::string_view pairingPrefix = "flock-pair-";
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
        throw std::runtime_error("Could not hash Session pairing data");
    return hex(bytes);
}

bool validId(std::string_view value, std::size_t max = 1024) {
    return !value.empty() && value.size() <= max &&
           std::ranges::none_of(value,[](unsigned char c) { return c <= 32 || c == 127; });
}

void authorize(const Access& access, const Person& person, std::string_view bot,
               const ThreadTarget& target, const CoordinationRequest* coordination, bool requireBot = true) {
    const auto operation = coordination ? Scope::sessionCoordinate(target.environmentId,target.threadId)
                                        : Scope::consultReply(target.environmentId,target.threadId);
    if (person.id.empty() || !access.permitsPerson(person,bot,Role::decide) ||
        !access.permitsPersonScope(person,Scope::sessionRead(target.environmentId,target.threadId)))
        throw ParticipationAccessDenied("Session participation access denied");
    if (!requireBot) return;
    if (!access.permits(bot,operation,true) ||
        (coordination && !access.permits(bot,Scope::sessionRead(target.environmentId,target.threadId),true)) ||
        (coordination && coordination->allowDelegation &&
         !access.permits(bot,Scope::workDelegate(target.environmentId,target.threadId),true)))
        throw Conflict("Session participation access denied");
}

std::optional<CoordinationRequest> permission(const Json& value) {
    if (value.is_null()) return {};
    return CoordinationRequest{value.at("goal").get<std::string>(),value.at("allowDelegation").get<bool>()};
}

}

std::string redactPairingCodes(std::string value) {
    std::size_t at = 0;
    while ((at = value.find(pairingPrefix,at)) != value.npos) {
        const auto first = at + pairingPrefix.size();
        std::size_t end = first;
        while (end < value.size() && std::string_view("0123456789abcdef").find(value[end]) != std::string_view::npos) ++end;
        if (end == first) {
            at = first;
            continue;
        }
        value.replace(at,end - at,"[pairing code]");
        at += std::string_view("[pairing code]").size();
    }
    return value;
}

void Store::Impl::initializeParticipationSchema() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS return_channels(
 id INTEGER PRIMARY KEY,
 environment_id TEXT NOT NULL, project_id TEXT NOT NULL, thread_id TEXT NOT NULL, run_id TEXT NOT NULL,
 request_id TEXT NOT NULL, source_message_id TEXT NOT NULL, bot_id TEXT NOT NULL, person_id TEXT NOT NULL,
 context TEXT NOT NULL, UNIQUE(environment_id,request_id))
)SQL");
    exec("CREATE INDEX IF NOT EXISTS return_channels_thread ON return_channels(environment_id,thread_id)");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS session_controls(
 channel_id INTEGER PRIMARY KEY REFERENCES return_channels(id),
 generation INTEGER NOT NULL DEFAULT 1,
 state TEXT NOT NULL CHECK(state IN ('active','paused','stopped','completed','failed','budget_exhausted')),
 auth_generation TEXT NOT NULL,
 permission TEXT NOT NULL,
 consult_command TEXT NOT NULL,
 event_cursor INTEGER NOT NULL,
 initial_pending INTEGER NOT NULL DEFAULT 1)
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS session_pairings(
 code_digest TEXT PRIMARY KEY,
 person_id TEXT NOT NULL, bot_id TEXT NOT NULL, environment_id TEXT NOT NULL, project_id TEXT NOT NULL,
 thread_id TEXT NOT NULL, auth_generation TEXT NOT NULL, cursor INTEGER NOT NULL, permission TEXT NOT NULL,
 expires_at INTEGER NOT NULL, used_channel INTEGER REFERENCES return_channels(id),
 client_request TEXT, request_digest TEXT)
)SQL");
}

Participation Store::Impl::authorizeParticipation(const Person& person, Id channel, bool requireBot) {
    auto row = query(R"SQL(
SELECT c.environment_id,c.project_id,c.thread_id,c.request_id,c.bot_id,c.person_id,c.context,
       s.permission,s.consult_command
FROM return_channels c JOIN session_controls s ON s.channel_id=c.id WHERE c.id=?
)SQL",{channel});
    if (!row.next() || person.id != row.string(5)) throw Conflict("Unknown Session participation");
    ThreadTarget target{row.string(0),row.string(1),row.string(2)};
    auto scope = permission(row.json(7));
    authorize(Access(*this),person,row.string(4),target,scope ? &*scope : nullptr,requireBot);
    ReturnChannel result{channel,{{std::move(target),row.string(3),row.string(4)},person.id},
                         row.json(6).at("request").get<std::string>()};
    if (!scope) return ConsultPermit{std::move(result),row.string(8)};
    return CoordinationActivation{std::move(result),{std::move(*scope)}};
}

Intake SessionHandoff::resolveBot(std::string_view bot) const {
    auto routing = intake_(bot,"");
    if (routing.botId != bot) throw Conflict("Session Bot is unavailable");
    return routing;
}

PairingTicket SessionHandoff::pair(const Person& person, const ThreadTarget& target, std::string_view bot,
                                   std::optional<CoordinationRequest> coordination) {
    if (!validId(target.environmentId) || !validId(target.projectId) || !validId(target.threadId) ||
        !validBotId(bot) || (coordination && (coordination->goal.size() > maxRequestBytes ||
                              trimSessionQuestionText(coordination->goal).empty())))
        throw Conflict("Invalid Session pairing");
    auto& db = *store_.p_;
    auto check = [&] {
        authorize(store_.access(),person,bot,target,coordination ? &*coordination : nullptr);
        db.sharedRedactor().requirePublic({person.id,bot,target.environmentId,target.projectId,target.threadId,
                                           coordination ? coordination->goal : ""});
        if (db.query("SELECT 1 FROM runs WHERE environment_id=? AND thread_id=?",
                     {target.environmentId,target.threadId}).next())
            throw Conflict("A Flock-owned Thread cannot be paired");
    };
    {
        std::lock_guard lock(db.mutex);
        check();
    }
    resolveBot(bot);
    const auto origin = session_.read(target,std::nullopt);
    if (!origin || origin->target != target || origin->authGeneration.empty())
        throw Conflict("The configured T3 connection cannot verify this Thread");

    std::array<unsigned char,32> bytes{};
    if (RAND_bytes(bytes.data(),static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error("Could not generate Session pairing code");
    PairingTicket ticket{std::string(pairingPrefix) + hex(bytes),unixTime() + 10 * 60};
    const auto scope = coordination ? Json{{"goal",redactPairingCodes(coordination->goal)},
                                             {"allowDelegation",coordination->allowDelegation}} : Json(nullptr);
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    check();
    db.exec("DELETE FROM session_pairings WHERE expires_at<=?",{unixTime()});
    db.exec(R"SQL(
INSERT INTO session_pairings(code_digest,person_id,bot_id,environment_id,project_id,thread_id,
auth_generation,cursor,permission,expires_at) VALUES(?,?,?,?,?,?,?,?,?,?)
)SQL",{digest(ticket.code),person.id,bot,target.environmentId,target.projectId,target.threadId,
        origin->authGeneration,origin->cursor,scope.dump(),ticket.expiresAt});
    tx.commit();
    return ticket;
}

HandoffAck SessionHandoff::intake(std::string_view code, std::string_view requestId, std::string_view text) {
    if (!code.starts_with(pairingPrefix) || code.size() != pairingPrefix.size() + 64 || !validId(code,128) ||
        !validId(requestId,128) || text.size() > maxRequestBytes || trimSessionQuestionText(text).empty())
        throw Conflict("Invalid Session request");
    const auto key = digest(code);
    const auto payload = digest(text);
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    auto row = db.query(R"SQL(
SELECT person_id,bot_id,environment_id,project_id,thread_id,auth_generation,cursor,permission,
       used_channel,client_request,request_digest FROM session_pairings
WHERE code_digest=? AND expires_at>?
)SQL",{key,unixTime()});
    if (!row.next()) throw Conflict("Session pairing is unavailable");
    const auto person = people_ ? people_(row.string(0)) : std::nullopt;
    if (!person || person->id != row.string(0)) throw Conflict("Session pairing is unavailable");
    const ThreadTarget target{row.string(2),row.string(3),row.string(4)};
    const auto bot = row.string(1);
    const auto scopeJson = row.json(7);
    const auto scope = permission(scopeJson);
    authorize(store_.access(),*person,bot,target,scope ? &*scope : nullptr);
    if (const auto used = row.number(8)) {
        if (row.string(9) != requestId || row.string(10) != payload) throw Conflict("Session pairing is already used");
        const auto existing = db.authorizeParticipation(*person,used);
        const auto reply = std::visit([](const auto& entry) {
            return HandoffAck{entry.channel.id,entry.channel.origin.invitation.requestId};
        },existing);
        tx.commit();
        return reply;
    }
    resolveBot(bot);
    if (db.query("SELECT 1 FROM runs WHERE environment_id=? AND thread_id=?",
                 {target.environmentId,target.threadId}).next())
        throw Conflict("A Flock-owned Thread cannot be paired");
    if (scope && db.query(R"SQL(
SELECT 1 FROM return_channels c JOIN session_controls s ON s.channel_id=c.id
WHERE c.environment_id=? AND c.thread_id=? AND s.permission<>'null' AND s.state IN ('active','paused')
)SQL",{target.environmentId,target.threadId}).next())
        throw Conflict("This Session already has a coordinator");

    const auto request = actionId();
    const Json context{{"request",redactPairingCodes(db.sharedRedactor().text(text))}};
    db.exec(R"SQL(
INSERT INTO return_channels(environment_id,project_id,thread_id,run_id,request_id,source_message_id,bot_id,person_id,context)
VALUES(?,?,?,'',?,'',?,?,?)
)SQL",{target.environmentId,target.projectId,target.threadId,request,bot,person->id,context.dump()});
    const auto channel = db.last();
    db.exec(R"SQL(
INSERT INTO session_controls(channel_id,state,auth_generation,permission,consult_command,event_cursor)
VALUES(?,'active',?,?,?,?)
)SQL",{channel,row.string(5),scopeJson.dump(),scope ? "" : actionId(),row.number(6)});
    db.exec("UPDATE session_pairings SET used_channel=?,client_request=?,request_digest=? WHERE code_digest=?",
            {channel,requestId,payload,key});
    tx.commit();
    return {channel,request};
}

}
