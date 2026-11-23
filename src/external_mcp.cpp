#include "flock/external_mcp.hpp"
#include "connector_credentials.hpp"
#include "storage.hpp"

namespace flockv0 {
namespace {
constexpr std::string_view tokenPrefix = "flock-mcp-";
constexpr std::string_view botUnavailable = "The Bot is unavailable to this connection";
constexpr std::string_view taskUnavailable = "The task is unavailable to this connection";

std::string redactConnectionTokens(std::string text) {
    return connector_credentials::redact(redactT3Tokens(std::move(text)),tokenPrefix,"[External MCP token]");
}
}

void Store::Impl::initializeExternalMcpSchema() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS external_mcp_connections(
 id TEXT PRIMARY KEY, person_id TEXT NOT NULL, token_digest TEXT UNIQUE
)
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS external_mcp_allowed_bots(
 connection_id TEXT NOT NULL REFERENCES external_mcp_connections(id), bot_id TEXT NOT NULL,
 PRIMARY KEY(connection_id,bot_id)
)
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS external_mcp_receipts(
 connection_id TEXT NOT NULL REFERENCES external_mcp_connections(id), request_id TEXT NOT NULL,
 request_digest TEXT NOT NULL, task_id INTEGER NOT NULL UNIQUE REFERENCES tasks(id),
 PRIMARY KEY(connection_id,request_id)
)
)SQL");
}

ExternalMcpCredential ExternalMcp::connect(std::string_view person, const std::vector<std::string>& bots) {
    if (bots.empty()) throw Conflict("At least one Bot is required");
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto actor = db.sessionActor(person);
    if (actor.id.empty()) throw Conflict("The connected person is unavailable");
    db.sharedRedactor().requirePublic({person,bots});
    ExternalMcpCredential credential{actionId(),connector_credentials::bearer(tokenPrefix)};
    db.exec("INSERT INTO external_mcp_connections VALUES(?,?,?)",
            {credential.connectionId,actor.id,connector_credentials::digest(credential.bearerToken)});
    for (const auto& bot : bots) {
        if (!store_.policy().allows(actor,Action::bot_decide,bot)) throw Conflict(std::string(botUnavailable));
        db.exec("INSERT OR IGNORE INTO external_mcp_allowed_bots VALUES(?,?)",{credential.connectionId,bot});
    }
    tx.commit();
    return credential;
}

void ExternalMcp::disconnect(std::string_view connectionId) {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    db.exec("UPDATE external_mcp_connections SET token_digest=NULL WHERE id=? AND token_digest IS NOT NULL",{connectionId});
    if (!sqlite3_changes(db.db)) throw Conflict("No active external MCP connection with this ID");
}

std::optional<ExternalMcpConnection> ExternalMcp::authenticate(std::string_view token) const {
    if (!connector_credentials::validBearer(token,tokenPrefix)) return std::nullopt;
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    auto row = db.query("SELECT id,person_id FROM external_mcp_connections WHERE token_digest=?",
                        {connector_credentials::digest(token)});
    if (!row.next() || db.sessionActor(row.string(1)).id.empty()) return std::nullopt;
    return ExternalMcpConnection{row.string(0)};
}

Id ExternalMcp::submit(const ExternalMcpConnection& connection, std::string_view bot, std::string_view requestId,
                       std::string_view text, const IntakeResolver& intake) {
    if (!validBotId(bot) || requestId.empty() || requestId.size() > 128 ||
        requestId.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-") != requestId.npos ||
        redactConnectionTokens(std::string(requestId)) != requestId ||
        text.size() > 8192 || text.find('\0') != text.npos || text.find_first_not_of(" \t\r\n") == text.npos)
        throw Conflict("Invalid task request");

    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    auto grant = db.query(R"SQL(
SELECT c.person_id FROM external_mcp_connections c JOIN external_mcp_allowed_bots b ON b.connection_id=c.id
WHERE c.id=? AND c.token_digest IS NOT NULL AND b.bot_id=?
)SQL",{connection.id,bot});
    if (!grant.next()) throw Conflict(std::string(botUnavailable));
    const auto actor = db.sessionActor(grant.string(0));
    if (!store_.policy().allows(actor,Action::bot_decide,bot)) throw Conflict(std::string(botUnavailable));

    const auto fingerprint = connector_credentials::digest(Json::array({bot,text}).dump());
    auto receipt = db.query("SELECT request_digest,task_id FROM external_mcp_receipts WHERE connection_id=? AND request_id=?",
                            {connection.id,requestId});
    if (receipt.next()) {
        if (receipt.string(0) != fingerprint) throw Conflict("This requestId already has different arguments");
        return receipt.number(1);
    }

    const auto scrub = db.sharedRedactor();
    scrub.requirePublic(requestId);
    const auto request = redactConnectionTokens(scrub.text(text));
    Intake item;
    try { item = intake(bot,request); }
    catch (const std::exception&) { throw Conflict(std::string(botUnavailable)); }
    if (item.botId != bot) throw Conflict(std::string(botUnavailable));
    item.request = request;
    item.environmentId = db.defaultT3Environment(bot,item.environmentId,{});
    if (item.environmentId.empty())
        throw Conflict("Connect T3 or configure a T3 environment for this Bot before submitting tasks");
    const auto task = db.submitRequest(item,&actor);
    db.exec("INSERT INTO external_mcp_receipts VALUES(?,?,?,?)",{connection.id,requestId,fingerprint,task});
    tx.commit();
    return task;
}

ExternalMcp::OwnedTask ExternalMcp::authorizeTask(const ExternalMcpConnection& connection, Id taskId) const {
    auto& db = *store_.p_;
    auto row = db.query(R"SQL(
SELECT c.person_id,t.bot_id FROM external_mcp_receipts r
JOIN external_mcp_connections c ON c.id=r.connection_id
JOIN tasks t ON t.id=r.task_id
JOIN external_mcp_allowed_bots b ON b.connection_id=c.id AND b.bot_id=t.bot_id
WHERE r.connection_id=? AND r.task_id=? AND c.token_digest IS NOT NULL
)SQL",{connection.id,taskId});
    if (!row.next()) throw Conflict(std::string(taskUnavailable));
    auto actor = db.sessionActor(row.string(0));
    auto bot = row.string(1);
    if (!store_.policy().allows(actor,Action::bot_decide,bot)) throw Conflict(std::string(taskUnavailable));
    return {std::move(actor),std::move(bot)};
}

Json ExternalMcp::status(const ExternalMcpConnection& connection, Id taskId) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db,Transaction::Mode::read);
    const auto owner = authorizeTask(connection,taskId);
    const auto task = db.load(taskId);
    const auto scrub = db.redactor(owner.bot);
    const auto publicText = [&](std::string_view text) { return redactConnectionTokens(scrub.text(text)); };
    Json data{{"taskId",taskId},{"bot",owner.bot},{"status",task.at("state")},
              {"question",nullptr},{"result",nullptr},{"sessionRun",nullptr}};
    const auto& wait = task.at("wait");
    if (wait.is_object()) {
        const auto reason = field(wait,"reason");
        data["waitReason"] = reason;
        if (task.at("state") == "waiting" && reason == "user_question" && db.waitIsActionable(task)) {
            auto choices = Json::array();
            for (const auto& choice : wait.value("choices",Json::array()))
                if (choice.is_string()) choices.push_back(publicText(choice.get_ref<const std::string&>()));
            const auto target = TaskTarget::from(task,wait.at("id"));
            data["question"] = {{"waitId",target.waitId},{"targetRevision",target.revision},
                                {"targetGeneration",target.generation},{"text",publicText(field(wait,"text"))},
                                {"choices",std::move(choices)}};
        }
    }
    if (task.at("state") == "completed") {
        auto row = db.query("SELECT json_extract(data,'$.text') FROM journal WHERE task_id=? AND kind='completed'"
                            " ORDER BY id DESC LIMIT 1",{taskId});
        if (row.next()) data["result"] = {{"text",publicText(row.string(0))}};
    }
    auto run = db.query(R"SQL(
SELECT r.environment_id,r.thread_id,r.link FROM runs r JOIN outbox o ON o.id=r.action_id
WHERE r.task_id=? ORDER BY o.rowid DESC LIMIT 1
)SQL",{taskId});
    if (run.next()) {
        Json session{{"environmentId",run.string(0)},{"threadId",run.string(1)},{"link",run.string(2)}};
        const auto encoded = session.dump();
        if (scrub.record(session,SecretRedactor::Fields::external) == session && redactConnectionTokens(encoded) == encoded)
            data["sessionRun"] = std::move(session);
    }
    tx.commit();
    return data;
}

void ExternalMcp::answer(const ExternalMcpConnection& connection, Id taskId, Id waitId, Id revision,
                         Id generation, std::string_view text) {
    if (text.find('\0') != text.npos) throw Conflict("Invalid task answer");
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto owner = authorizeTask(connection,taskId);
    db.operate(owner.actor,{owner.bot,taskId,waitId,revision,generation},"answer",
               {{"text",redactConnectionTokens(std::string(text))}},0,InboxKind::user_question);
    tx.commit();
}
}
