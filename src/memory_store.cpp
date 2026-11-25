#include "storage.hpp"

#include <algorithm>

namespace flockv0 {
namespace {
constexpr std::size_t memoryBytes = 8192;

struct MemoryDocument {
    std::string content;
    Id version = 0;
    Seconds updatedAt = 0;
};

std::string privateKey(std::string_view bot) {
    return "bot:" + std::string(bot) + ":memory";
}

std::optional<MemoryDocument> findMemory(sqlite3* db, std::string_view key) {
    Statement row(db,"SELECT content,version,updated_at FROM memory_documents WHERE doc_key=?",{key});
    if (!row.next()) return std::nullopt;
    return MemoryDocument{row.string(0),row.number(1),row.number(2)};
}

std::vector<std::string_view> lines(std::string_view content) {
    std::vector<std::string_view> result;
    while (!content.empty()) {
        const auto end = content.find('\n');
        const auto length = end == content.npos ? content.size() : end + 1;
        result.push_back(content.substr(0,length));
        content.remove_prefix(length);
    }
    return result;
}

std::string diffSummary(std::string_view before, std::string_view after) {
    const auto oldLines = lines(before), newLines = lines(after);
    const auto prefix = std::mismatch(oldLines.begin(),oldLines.end(),newLines.begin(),newLines.end());
    const auto suffix = std::mismatch(oldLines.rbegin(),std::make_reverse_iterator(prefix.first),
                                      newLines.rbegin(),std::make_reverse_iterator(prefix.second));
    return "-" + std::to_string(std::distance(prefix.first,suffix.first.base())) +
        " +" + std::to_string(std::distance(prefix.second,suffix.second.base())) + " lines";
}

void checkVersion(Id expected, Id current) {
    if (expected != current)
        throw Conflict("Memory version conflict: expected " + std::to_string(expected) +
                       ", current " + std::to_string(current));
}

Json revisionView(const Statement& row) {
    return {{"id",row.number(0)},{"version",row.number(1)},{"baseVersion",row.number(2)},
            {"author",row.string(3)},{"diffSummary",row.string(4)},{"createdAt",row.number(5)},
            {"revertedFrom",row.number(6) ? Json(row.number(6)) : Json(nullptr)}};
}
}

void Store::Impl::initializeMemorySchema() {
    if (sqlite3_exec(db,R"SQL(
CREATE TABLE IF NOT EXISTS memory_documents(
  doc_key TEXT PRIMARY KEY, scope_kind TEXT NOT NULL, owner_bot TEXT, doc_id TEXT NOT NULL,
  content TEXT NOT NULL, version INTEGER NOT NULL, updated_at INTEGER NOT NULL,
  CHECK((scope_kind='private' AND owner_bot IS NOT NULL) OR (scope_kind='shared' AND owner_bot IS NULL)));
CREATE TABLE IF NOT EXISTS memory_revisions(
  id INTEGER PRIMARY KEY, doc_key TEXT NOT NULL REFERENCES memory_documents(doc_key),
  version INTEGER NOT NULL, base_version INTEGER NOT NULL, author TEXT NOT NULL,
  diff_summary TEXT NOT NULL, previous_content TEXT NOT NULL, new_content TEXT NOT NULL,
  reverted_from INTEGER REFERENCES memory_revisions(id), created_at INTEGER NOT NULL,
  UNIQUE(doc_key,version));
CREATE INDEX IF NOT EXISTS memory_revisions_document ON memory_revisions(doc_key,id DESC);
CREATE TABLE IF NOT EXISTS memory_proposals(
  id INTEGER PRIMARY KEY, doc_key TEXT NOT NULL REFERENCES memory_documents(doc_key),
  base_version INTEGER NOT NULL, proposed_content TEXT NOT NULL, rationale TEXT NOT NULL,
  author TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('pending','applied','rejected')),
  created_at INTEGER NOT NULL, decided_by TEXT, decided_at INTEGER,
  diff_summary TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS memory_proposals_document ON memory_proposals(doc_key,id DESC);
)SQL",nullptr,nullptr,nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}

BotNotes Store::Impl::loadNotes(std::string_view bot, const Runtime::SoulLoader& soul, Seconds now) {
    BotNotes notes{soul ? bounded(soul(bot),memoryBytes) : std::string{},{}};
    const auto key = privateKey(bot);
    std::lock_guard lock(mutex);
    if (auto doc = findMemory(db,key)) {
        notes.memory = std::move(doc->content);
        return notes;
    }
    Transaction tx(db);
    if (auto doc = findMemory(db,key)) notes.memory = std::move(doc->content);
    else notes.memory = seedPrivateMemory(key,bot,redactor(bot),now);
    tx.commit();
    return notes;
}

std::optional<SessionMemory> Store::Impl::sessionMemory(const Command& command, Seconds now) {
    if (command.instruction.size() >= maxSessionPromptBytes) return std::nullopt;
    const auto& bot = command.ownerBotId;
    const auto key = privateKey(bot);
    const auto doc = findMemory(db,key);
    const auto version = doc ? doc->version : Id{0};
    const auto scrub = redactor(bot);
    auto excerpt = doc ? scrub.text(doc->content) : seedPrivateMemory(key,bot,scrub,now);
    if (excerpt.find_first_not_of(" \t\r\n") == excerpt.npos) return std::nullopt;
    const auto marker = sessionPrompt(command.actionId,{});
    const auto bytes = std::min(memoryBytes,maxSessionPromptBytes - command.instruction.size());
    const auto label = field(command.config,"botDisplayName");
    const auto heading = "\n\nNotes from " + bounded(scrub.text(label.empty() ? bot : label),128) + "'s memory:\n";
    constexpr std::string_view truncation = "\n[Bot memory truncated]";
    const bool truncated = heading.size() + excerpt.size() > bytes;
    if (truncated) {
        if (bytes <= heading.size() + truncation.size()) return std::nullopt;
        excerpt = bounded(excerpt,bytes - heading.size() - truncation.size());
        if (excerpt.empty()) return std::nullopt;
    }
    auto section = heading + excerpt;
    if (truncated) section += truncation;
    if (command.tool == "follow_up") {
        auto prior = query(R"SQL(
SELECT json_extract(o.data,'$.sessionMemory.section'),json_extract(o.data,'$.sessionMemory.excerptBytes')
FROM runs r JOIN tasks t ON t.id=r.task_id JOIN outbox o ON o.id=r.action_id
WHERE r.environment_id=? AND r.thread_id=? AND t.bot_id=?
  AND json_extract(o.data,'$.sessionMemory.version')=?
)SQL",{command.environmentId,command.target.threadId,bot,version});
        const auto prefix = std::string_view(section).substr(0,heading.size() + excerpt.size());
        while (prior.next()) {
            const auto saved = prior.string(0);
            if (saved == section || (prior.number(1) >= static_cast<Id>(excerpt.size()) && saved.starts_with(prefix)))
                return std::nullopt;
        }
    }
    const auto base = command.instruction.substr(0,command.instruction.size() - marker.size());
    if (scrub.containsProtectedText(base + section + marker)) return std::nullopt;
    return SessionMemory{version,std::move(section),excerpt.size()};
}

void Store::Impl::createMemory(std::string_view key, std::string_view bot, std::string_view id,
                              std::string_view content, Seconds now) {
    exec(R"SQL(
INSERT INTO memory_documents(doc_key,scope_kind,owner_bot,doc_id,content,version,updated_at)
VALUES(?,?,?,?,?,0,?)
)SQL",{key,bot.empty() ? "shared" : "private",bot.empty() ? Json(nullptr) : Json(bot),id,content,now});
}

std::string Store::Impl::seedPrivateMemory(std::string_view key, std::string_view bot, const SecretRedactor& scrub,
                                          Seconds now) {
    auto content = memorySeed ? bounded(scrub.text(memorySeed(bot)),memoryBytes) : std::string{};
    createMemory(key,bot,"memory",content,now);
    return content;
}

Json Store::memory(const Person& actor, std::string_view bot, std::string_view operation, const Json& args) {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    auto result = p_->memory(bot,actor,operation,args,unixTime());
    if (result.is_null()) throw Conflict("Unknown memory operation");
    tx.commit();
    return result;
}

Json Store::Impl::memory(std::string_view bot, const Person& actor, std::string_view operation, const Json& args,
                         Seconds now, bool fromBot) {
    const bool reading = operation == "read_memory" || operation == "memory_history";
    if (!reading && operation != "propose_memory_edit" && operation != "apply_memory_edit" &&
        operation != "revert_memory_edit" && !(!fromBot && operation == "reject_memory_edit")) return nullptr;
    if (!validBotId(bot) || !args.is_object()) throw Conflict("Invalid memory arguments");
    const bool shared = args.contains("docId");
    const auto id = shared ? field(args,"docId") : "memory";
    const auto action = reading ? Action::shared_memory_read : Action::shared_memory_write;
    auto access = Policy(*this);
    if (!access.allows(actor,reading ? Action::bot_view : Action::bot_settings,bot) ||
        (shared && (!access.allows(actor,action,id) ||
                    (fromBot ? access.forbidden(bot,action,id) : !access.allows(bot,action,id)))))
        throw AccessDenied("Memory access denied");

    const auto key = shared ? "shared:" + id : privateKey(bot);
    const auto author = fromBot ? "bot:" + std::string(bot) : "person:" + actor.id;
    const auto scrub = shared ? sharedRedactor() : redactor(bot);
    scrub.requirePublic({key,author});
    auto doc = findMemory(db,key);
    if (!doc && !shared) doc = MemoryDocument{seedPrivateMemory(key,bot,scrub,now),0,now};
    const auto [content,version,updatedAt] = doc.value_or(MemoryDocument{});

    if (operation == "read_memory")
        return scrub.record({{"docKey",key},{"content",content},{"version",version},{"updatedAt",updatedAt}});
    if (operation == "memory_history") {
        if (args.contains("revisionId")) {
            auto row = query(R"SQL(
SELECT id,version,base_version,author,diff_summary,created_at,reverted_from,previous_content,new_content
FROM memory_revisions WHERE doc_key=? AND id=?
)SQL",{key,requiredInteger(args,"revisionId")});
            if (!row.next()) throw Conflict("Memory revision not found");
            auto detail = revisionView(row);
            detail["previousContent"] = row.string(7);
            detail["newContent"] = row.string(8);
            return scrub.record(std::move(detail));
        }
        const Id proposal = args.contains("proposalId") ? requiredInteger(args,"proposalId") : 0;
        const auto proposalsBefore = args.contains("proposalsBefore") ? requiredInteger(args,"proposalsBefore") : 0;
        auto proposals = query(R"SQL(
SELECT id,base_version,rationale,author,status,created_at,decided_by,decided_at,diff_summary,proposed_content
FROM memory_proposals WHERE doc_key=? AND (?=0 OR id=?) AND (?=0 OR id<?) ORDER BY id DESC LIMIT 21
)SQL",{key,proposal,proposal,proposalsBefore,proposalsBefore});
        Json edits = Json::array();
        Id nextProposal = 0;
        while (proposals.next()) {
            if (edits.size() == 20) {
                nextProposal = edits.back().at("id");
                break;
            }
            Json row = {{"id",proposals.number(0)},{"baseVersion",proposals.number(1)},{"rationale",proposals.string(2)},
                        {"author",proposals.string(3)},{"status",proposals.string(4)},{"createdAt",proposals.number(5)},
                        {"decidedBy",proposals.string(6)},{"decidedAt",proposals.number(7)},{"diffSummary",proposals.string(8)}};
            if (proposal) {
                row["proposedContent"] = proposals.string(9);
                return scrub.record(std::move(row));
            }
            edits.push_back(std::move(row));
        }
        if (proposal) throw Conflict("Memory proposal not found");
        const auto before = args.contains("before") ? requiredInteger(args,"before") : 0;
        auto revisions = query(R"SQL(
SELECT id,version,base_version,author,diff_summary,created_at,reverted_from
FROM memory_revisions WHERE doc_key=? AND (?=0 OR id<?) ORDER BY id DESC LIMIT 21
)SQL",{key,before,before});
        Json history = Json::array();
        Id next = 0;
        while (revisions.next()) {
            if (history.size() == 20) {
                next = history.back().at("id");
                break;
            }
            history.push_back(revisionView(revisions));
        }
        return scrub.record({{"docKey",key},{"version",version},{"revisions",history},{"proposals",edits},{"before",next},{"proposalsBefore",nextProposal}});
    }

    const auto proposal = args.contains("proposalId") ? requiredInteger(args,"proposalId") : 0;
    if (operation == "reject_memory_edit") {
        if (!proposal) throw Conflict("A person and a proposal ID are required");
        exec("UPDATE memory_proposals SET status='rejected',decided_by=?,decided_at=? WHERE doc_key=? AND id=? AND status='pending'",
             {author,now,key,proposal});
        if (!sqlite3_changes(db)) throw Conflict("Pending memory proposal not found");
        return {{"proposalId",proposal},{"status","rejected"}};
    }
    const auto expected = requiredInteger(args,"baseVersion",0);
    checkVersion(expected,version);
    std::string updated;
    Id reverted = 0;
    if (operation == "revert_memory_edit") {
        reverted = requiredInteger(args,"revisionId");
        auto row = query("SELECT previous_content FROM memory_revisions WHERE doc_key=? AND id=?",{key,reverted});
        if (!row.next()) throw Conflict("Memory revision not found");
        updated = row.string(0);
    } else if (operation == "apply_memory_edit" && proposal) {
        if (args.contains("content")) throw Conflict("Choose either content or proposalId");
        auto row = query("SELECT base_version,proposed_content FROM memory_proposals WHERE doc_key=? AND id=? AND status='pending'",
                         {key,proposal});
        if (!row.next()) throw Conflict("Pending memory proposal not found");
        checkVersion(row.number(0),version);
        updated = row.string(1);
    } else {
        if (!args.contains("content") || !args.at("content").is_string() ||
            args.at("content").get_ref<const std::string&>().size() > memoryBytes)
            throw Conflict("Memory content must be text up to 8,192 bytes");
        updated = field(args,"content");
    }

    updated = scrub.text(updated);
    if (updated.size() > memoryBytes) throw Conflict("Redacted memory content exceeds 8,192 bytes");
    const auto summary = diffSummary(content,updated);
    if (!doc) createMemory(key,{},id,{},now);
    if (operation == "propose_memory_edit") {
        const auto rationale = field(args,"rationale");
        if (rationale.empty() || rationale.size() > 1000) throw Conflict("Enter a rationale up to 1,000 bytes");
        exec(R"SQL(
INSERT INTO memory_proposals(doc_key,base_version,proposed_content,rationale,author,status,created_at,diff_summary)
VALUES(?,?,?,?,?,'pending',?,?)
)SQL",{key,version,updated,scrub.text(rationale),author,now,summary});
        return {{"docKey",key},{"proposalId",last()},{"baseVersion",version},{"diffSummary",summary},{"status","pending"}};
    }

    exec("UPDATE memory_documents SET content=?,version=version+1,updated_at=? WHERE doc_key=? AND version=?",
         {updated,now,key,expected});
    if (!sqlite3_changes(db)) throw Conflict("Memory version conflict");
    exec(R"SQL(
INSERT INTO memory_revisions(doc_key,version,base_version,author,diff_summary,previous_content,new_content,reverted_from,created_at)
VALUES(?,?,?,?,?,?,?,?,?)
)SQL",{key,version+1,version,author,summary,content,updated,reverted ? Json(reverted) : Json(nullptr),now});
    const auto revision = last();
    if (operation == "apply_memory_edit" && proposal)
        exec("UPDATE memory_proposals SET status='applied',decided_by=?,decided_at=? WHERE id=?",{author,now,proposal});
    return {{"docKey",key},{"revisionId",revision},{"version",version+1},{"diffSummary",summary}};
}
}
