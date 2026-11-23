#include "flock/session_handoff.hpp"
#include "storage.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <thread>
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

bool validText(std::string_view value) {
    return value.size() <= maxRequestBytes && value.find('\0') == value.npos &&
           !trimSessionQuestionText(value).empty();
}

std::optional<CoordinationRequest> permission(const Json& value) {
    if (value.is_null()) return {};
    return CoordinationRequest{value.at("goal").get<std::string>(),value.at("allowDelegation").get<bool>()};
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
CREATE TABLE IF NOT EXISTS return_channels(
 id INTEGER PRIMARY KEY,
 environment_id TEXT NOT NULL, project_id TEXT NOT NULL, thread_id TEXT NOT NULL, run_id TEXT NOT NULL,
 request_id TEXT NOT NULL, source_message_id TEXT NOT NULL, bot_id TEXT NOT NULL, person_id TEXT NOT NULL,
 context TEXT NOT NULL, client_request_id TEXT NOT NULL, request_digest TEXT NOT NULL,
 UNIQUE(environment_id,request_id), UNIQUE(environment_id,client_request_id))
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
CREATE TABLE IF NOT EXISTS t3_connector(
 singleton INTEGER PRIMARY KEY CHECK(singleton=1), id TEXT NOT NULL, person_id TEXT NOT NULL,
 token_digest TEXT NOT NULL, port INTEGER NOT NULL)
)SQL");
    exec("CREATE TABLE IF NOT EXISTS t3_connected_environments(environment_id TEXT PRIMARY KEY)");
}

std::string T3Connections::connect(std::string_view environment, std::string_view person,
                                  unsigned port) {
    if (!validId(environment) || !validId(person) || port == 0 || port > 65535)
        throw Conflict("Invalid T3 connection");
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto rotate = [&] {
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
    tx.commit();
    return token;
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

std::optional<T3ConnectionSettings> T3Connections::settings(std::string_view person) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    auto rows = db.query("SELECT port,environment_id FROM t3_connector CROSS JOIN t3_connected_environments"
                         " WHERE person_id=? ORDER BY environment_id",{person});
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

bool matchesThreadInvocation(const Json& item, std::string_view threadId,
                             std::string_view activeRunId, const Json& arguments) {
    if (activeRunId.empty() || !item.is_object() ||
        item.value("type",Json(nullptr)) != "dynamic_tool" ||
        item.value("visibility",Json(nullptr)) != "local" ||
        item.value("sourceThreadId",Json(nullptr)) != threadId ||
        item.value("runId",Json(nullptr)) != activeRunId ||
        item.value("textTruncated",Json(false)) != false) return false;
    const auto text = item.find("text");
    if (text == item.end() || !text->is_string()) return false;
    const auto call = Json::parse(text->get_ref<const std::string&>(),nullptr,false);
    if (!call.is_object()) return false;
    const auto tool = call.find("toolName");
    if (tool == call.end() || !tool->is_string()) return false;
    const auto& name = tool->get_ref<const std::string&>();
    if (name != "invoke_flock_bot" && name != "flock.invoke_flock_bot" && !name.ends_with("__invoke_flock_bot"))
        return false;
    auto input = call.value("input",Json(nullptr));
    if (input.is_string()) input = Json::parse(input.get_ref<const std::string&>(),nullptr,false);
    return input.is_object() && input == arguments;
}

Participation Store::Impl::authorizeParticipation(const Person& person, Id channel, bool requireConnection) {
    auto row = query(R"SQL(
SELECT c.environment_id,c.project_id,c.thread_id,c.request_id,c.bot_id,c.person_id,c.context,
       s.permission,s.consult_command,t.id
FROM return_channels c JOIN session_controls s ON s.channel_id=c.id
LEFT JOIN t3_connected_environments e ON e.environment_id=c.environment_id
LEFT JOIN t3_connector t ON t.person_id=c.person_id AND e.environment_id IS NOT NULL WHERE c.id=?
)SQL",{channel});
    if (!row.next() || person.id != row.string(5) || !Policy(*this).allows(person,Action::bot_decide,row.string(4)))
        throw Conflict("Session participation access denied");
    ThreadTarget target{row.string(0),row.string(1),row.string(2)};
    auto scope = permission(row.json(7));
    if (requireConnection && row.string(9).empty())
        throw Conflict("T3 connection no longer permits this handoff");
    ReturnChannel result{channel,{{std::move(target),row.string(3),row.string(4)},person.id},
                         row.json(6).at("request").get<std::string>()};
    if (requireConnection && scope && !Policy(*this).allows(row.string(4),Action::t3_coordinate,row.string(0)))
        throw Conflict("T3 coordination denied by installation policy");
    if (!scope) return ConsultPermit{std::move(result),row.string(8)};
    return CoordinationActivation{std::move(result),{std::move(*scope)}};
}

Intake SessionHandoff::resolveBot(std::string_view bot) const {
    auto routing = intake_(bot,"");
    if (routing.botId != bot) throw Conflict("Session Bot is unavailable");
    return routing;
}

std::optional<Person> SessionHandoff::person(const T3Connection& connection) const {
    return people_ ? people_(connection.personId) : std::nullopt;
}

HandoffAck SessionHandoff::invoke(const T3Connection& connection, const Json& arguments,
                                  const std::function<bool(std::string_view)>& botExists,
                                  std::chrono::milliseconds proofTimeout) {
    if (!arguments.is_object()) throw Conflict("Invalid invocation arguments");
    const auto string = [&](std::string_view key) {
        const auto value = arguments.find(key);
        if (value == arguments.end() || !value->is_string()) throw Conflict("Invalid invocation argument: " + std::string(key));
        return value->get<std::string>();
    };
    const auto thread = string("threadId"), bot = string("botId"), request = string("requestId"), text = string("text");
    const auto mode = arguments.contains("mode") ? string("mode") : "consult";
    const auto goal = arguments.contains("goal") ? string("goal") : "";
    if (arguments.size() != 4u + arguments.contains("mode") + arguments.contains("goal") ||
        !validId(thread) || !validBotId(bot) || request.empty() || request.size() > 128 ||
        request.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-") != request.npos ||
        !validText(text) || (mode != "consult" && mode != "coordinate") ||
        ((mode == "coordinate" || arguments.contains("goal")) && !validText(goal)))
        throw Conflict("Invalid invocation arguments");
    const auto coordination = mode == "coordinate" ? std::optional(CoordinationRequest{goal,true}) : std::nullopt;
    const auto actor = person(connection);
    if (!botExists(bot)) throw Conflict("Unknown Bot");
    if (!actor || actor->id != connection.personId || !store_.policy().allows(*actor,Action::bot_decide,bot))
        throw Conflict("Bot decision access is required");

    const auto settings = T3Connections(store_).settings(connection.personId);
    if (!settings) throw Conflict("T3 connection no longer permits this handoff");
    const auto& environments = settings->environments;
    const auto deadline = std::chrono::steady_clock::now() + proofTimeout;
    do {
        const auto probe = [&](std::string_view environment) -> std::optional<ThreadProof> {
            auto proof = session_.proveInvocation(environment,thread,arguments,deadline);
            if (proof && proof->target.environmentId == environment && proof->target.threadId == thread &&
                validId(proof->target.projectId) && !proof->activeRunId.empty() && !proof->authGeneration.empty() &&
                !proof->itemId.empty()) return proof;
            return {};
        };
        std::optional<ThreadProof> proof;
        if (environments.size() == 1) {
            proof = probe(environments.front());
        } else {
            std::vector<std::future<std::optional<ThreadProof>>> probes;
            for (const auto& environment : environments)
                probes.push_back(std::async(std::launch::async,probe,std::string_view(environment)));
            for (auto& candidate : probes) {
                if (auto match = candidate.get()) {
                    if (proof) throw Conflict("This invocation matched more than one T3 environment");
                    proof = std::move(match);
                }
            }
        }
        if (proof) {
            if (!botExists(bot)) throw Conflict("Unknown Bot");
            return openChannel(connection,*proof,bot,request,text,digest(arguments.dump()),coordination);
        }
        std::this_thread::sleep_until(std::min(deadline,std::chrono::steady_clock::now()+std::chrono::milliseconds(150)));
    } while (std::chrono::steady_clock::now() < deadline);
    throw Conflict("Could not verify this invocation in the active T3 Thread. Use currentThreadId from t3_thread_list.");
}

HandoffAck SessionHandoff::openChannel(const T3Connection& connection, const ThreadProof& proof, std::string_view bot,
                                       std::string_view requestId, std::string_view text, std::string_view payloadDigest,
                                       const std::optional<CoordinationRequest>& scope) {
    const auto& target = proof.target;
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto actor = person(connection);
    auto current = db.query("SELECT id FROM t3_connector WHERE id=? AND person_id=? AND "
                            "EXISTS(SELECT 1 FROM t3_connected_environments WHERE environment_id=?)",
                            {connection.id,connection.personId,target.environmentId});
    if (!actor || actor->id != connection.personId || !store_.policy().allows(*actor,Action::bot_decide,bot) ||
        !current.next())
        throw Conflict("T3 connection or Bot access changed");
    if (scope && !store_.policy().allows(bot,Action::t3_coordinate,target.environmentId))
        throw Conflict("T3 coordination denied by installation policy");
    if (db.query("SELECT 1 FROM runs WHERE environment_id=? AND thread_id=?",
                 {target.environmentId,target.threadId}).next())
        throw Conflict("A Flock-owned Thread cannot open a handoff");

    auto existing = db.query(R"SQL(
SELECT id,request_id,request_digest,person_id,project_id FROM return_channels
WHERE environment_id=? AND client_request_id=?
)SQL",{target.environmentId,requestId});
    if (existing.next()) {
        if (existing.string(2) != payloadDigest || existing.string(3) != connection.personId ||
            existing.string(4) != target.projectId)
            throw Conflict("This requestId is already in use with different arguments");
        HandoffAck ack{existing.number(0),existing.string(1)};
        tx.commit();
        return ack;
    }
    try { resolveBot(bot); }
    catch (const Conflict&) { throw Conflict("Session Bot is unavailable"); }
    if (scope && db.query(R"SQL(
SELECT 1 FROM return_channels c JOIN session_controls s ON s.channel_id=c.id
WHERE c.environment_id=? AND c.thread_id=? AND s.permission<>'null' AND s.state IN ('active','paused')
)SQL",{target.environmentId,target.threadId}).next())
        throw Conflict("This Session already has a coordinator");

    const auto scrub = db.sharedRedactor();
    const auto publicIds = {std::string_view(connection.personId),bot,std::string_view(target.environmentId),
                            std::string_view(target.projectId),std::string_view(target.threadId),requestId};
    for (const auto id : publicIds) {
        scrub.requirePublic(id);
        if (redactT3Tokens(std::string(id)) != id) throw Conflict("Invalid handoff identifier");
    }
    const auto request = actionId();
    const Json context{{"request",redactT3Tokens(scrub.text(text))}};
    const Json scopeJson = scope ? Json{{"goal",redactT3Tokens(scrub.text(scope->goal))},
                                       {"allowDelegation",scope->allowDelegation}} : Json(nullptr);
    db.exec(R"SQL(
INSERT INTO return_channels(environment_id,project_id,thread_id,run_id,request_id,source_message_id,
                            bot_id,person_id,context,client_request_id,request_digest)
VALUES(?,?,?,?,?,?,?,?,?,?,?)
)SQL",{target.environmentId,target.projectId,target.threadId,proof.activeRunId,request,proof.itemId,
        bot,connection.personId,context.dump(),requestId,payloadDigest});
    const auto channel = db.last();
    db.exec(R"SQL(
INSERT INTO session_controls(channel_id,state,auth_generation,permission,consult_command,event_cursor)
VALUES(?,'active',?,?,?,?)
)SQL",{channel,proof.authGeneration,scopeJson.dump(),scope ? "" : actionId(),proof.cursor});
    tx.commit();
    return {channel,request};
}

}
