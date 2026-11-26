#include "flock/bot_memory.hpp"
#include "flock/bot_id.hpp"
#include "conversation_turns.hpp"
#include "memory_learnings.hpp"
#include "memory_storage.hpp"
#include "flock/outbox.hpp"

#include "conversation_storage.hpp"
#include "database.hpp"
#include "secret_storage.hpp"
#include "flock/store_options.hpp"
#include "flock/bot_profile.hpp"
#include "flock/limits.hpp"
#include "flock/memory_automation.hpp"
#include "flock/policy.hpp"
#include "flock/secrets.hpp"
#include "flock/types.hpp"
#include "flock/person.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/task.hpp"
#include "json_fields.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sqlite3.h>

namespace flockv0 {
namespace {
struct MemoryDocument {
    std::string content;
    Id version = 0;
    Seconds updatedAt = 0;
};

std::string privateKey(std::string_view bot) {
    return "bot:" + std::string(bot) + ":memory";
}

std::optional<MemoryDocument> findMemory(sqlite3* db, std::string_view key) {
    Statement row(db, "SELECT content,version,updated_at FROM memory_documents WHERE doc_key=?", {key});
    if (!row.next()) return std::nullopt;
    return MemoryDocument{row.string(0), row.number(1), row.number(2)};
}

std::vector<std::string_view> lines(std::string_view content) {
    std::vector<std::string_view> result;
    while (!content.empty()) {
        const auto end = content.find('\n');
        const auto length = end == content.npos ? content.size() : end + 1;
        result.push_back(content.substr(0, length));
        content.remove_prefix(length);
    }
    return result;
}

std::string diffSummary(std::string_view before, std::string_view after) {
    const auto oldLines = lines(before), newLines = lines(after);
    const auto prefix = std::mismatch(oldLines.begin(), oldLines.end(), newLines.begin(), newLines.end());
    const auto suffix = std::mismatch(oldLines.rbegin(), std::make_reverse_iterator(prefix.first), newLines.rbegin(),
                                      std::make_reverse_iterator(prefix.second));
    return "-" + std::to_string(std::distance(prefix.first, suffix.first.base())) + " +" +
           std::to_string(std::distance(prefix.second, suffix.second.base())) + " lines";
}

void checkVersion(Id expected, Id current) {
    if (expected != current)
        throw Conflict("Memory version conflict: expected " + std::to_string(expected) + ", current " + std::to_string(current));
}

Json revisionView(const Statement& row) {
    Json revision{{"id", row.number(0)},
                  {"version", row.number(1)},
                  {"baseVersion", row.number(2)},
                  {"author", row.string(3)},
                  {"diffSummary", row.string(4)},
                  {"createdAt", row.number(5)},
                  {"revertedFrom", row.number(6) ? Json(row.number(6)) : Json(nullptr)}};
    if (!row.null(7)) revision["source"] = row.json(7);
    if (!row.null(8) && !row.null(9)) {
        const auto notice = row.json(8);
        revision["learning"] = {{"taskTitle", notice.at("taskTitle")},
                                {"requestEventId", notice.at("requestEventId")},
                                {"reportEventId", notice.at("reportEventId")},
                                {"added", notice.at("added")},
                                {"removed", notice.at("removed")},
                                {"run", row.json(9)}};
    }
    return revision;
}

Json writeMemoryRevision(Database& database, std::string_view key, const MemoryDocument& before, std::string_view updated,
                         std::string_view author, Seconds now, Id reverted = 0, const Json& source = nullptr) {
    const auto summary = diffSummary(before.content, updated);
    database.exec("UPDATE memory_documents SET content=?,version=version+1,updated_at=? WHERE doc_key=? AND version=?",
                  {updated, now, key, before.version});
    if (!sqlite3_changes(database.db)) throw Conflict("Memory version conflict");
    database.exec(R"SQL(
INSERT INTO memory_revisions(doc_key,version,base_version,author,diff_summary,previous_content,new_content,reverted_from,created_at,source)
VALUES(?,?,?,?,?,?,?,?,?,?)
)SQL",
                  {key, before.version + 1, before.version, author, summary, before.content, updated,
                   reverted ? Json(reverted) : Json(nullptr), now, source});
    return {{"docKey", key}, {"revisionId", database.last()}, {"version", before.version + 1}, {"diffSummary", summary}};
}

std::string roleHash(std::string_view text) {
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    if (EVP_Digest(text.data(), text.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 || size != digest.size())
        throw Conflict("Could not hash the Bot's Role");
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

SessionContext lastSessionContext(Database& database, const SessionContextTarget& target) {
    SessionContext context;
    if (target.threadId.empty()) return context;
    auto prior = database.query(R"SQL(
SELECT t.bot_id,r.cancelled_before_start,
       o.role_hash,o.role_section,o.role_excerpt_bytes,o.memory_version,o.memory_section,o.memory_excerpt_bytes
FROM runs r JOIN tasks t ON t.id=r.task_id JOIN outbox o ON o.id=r.action_id
WHERE r.environment_id=? AND r.thread_id=?
ORDER BY r.rowid DESC
)SQL",
                                {target.environmentId, target.threadId});
    while ((!context.role || !context.memory) && prior.next()) {
        if (prior.string(0) != target.bot) break;
        if (prior.number(1)) continue;
        if (!context.role && !prior.null(2))
            context.role = SessionRole{prior.string(2), prior.string(3), static_cast<std::size_t>(prior.number(4))};
        if (!context.memory && !prior.null(5))
            context.memory = SessionMemory{prior.number(5), prior.string(6), static_cast<std::size_t>(prior.number(7))};
    }
    return context;
}

struct SessionSectionText {
    std::string heading;
    std::string_view cleared, truncated;
};

template <typename Version>
std::optional<SessionContextSection<Version>> sessionExcerpt(Version version, std::string_view content, const SessionSectionText& text,
                                                             const std::optional<SessionContextSection<Version>>& prior,
                                                             std::size_t budget) {
    constexpr std::string_view separator = "\n\n";
    if (content.find_first_not_of(" \t\r\n") == content.npos) {
        if (!prior) return std::nullopt;
        content = text.cleared;
    }
    const auto overhead = text.heading.size() + separator.size();
    if (budget <= overhead) return std::nullopt;
    auto excerptBytes = budget - overhead;
    const bool truncated = content.size() > excerptBytes;
    if (truncated) {
        if (excerptBytes <= text.truncated.size()) return std::nullopt;
        excerptBytes -= text.truncated.size();
    }
    const auto excerpt = bounded(content, excerptBytes);
    if (excerpt.empty()) return std::nullopt;
    auto section = text.heading + excerpt;
    if (prior && prior->version == version && prior->excerptBytes >= excerpt.size() && prior->section.starts_with(section))
        return std::nullopt;
    if (truncated) section += text.truncated;
    section += separator;
    if (prior && prior->section == section) return std::nullopt;
    return SessionContextSection<Version>{std::move(version), std::move(section), excerpt.size()};
}
}

MemoryStorage::MemoryStorage(Database& database, const SecretStorage& secretStorage, Policy& permissions,
                             ConversationStorage& conversationTurns, MemorySeed seed)
    : database_(database), secretStorage_(secretStorage), permissions(permissions), conversationTurns(conversationTurns),
      seed_(std::move(seed)) {}

BotNotes MemoryStorage::loadNotes(std::string_view bot, const SoulLoader& soul, Seconds now) {
    BotNotes notes{soul ? bounded(soul(bot), maxBotRoleBytes) : std::string{}, {}};
    const auto key = privateKey(bot);
    std::lock_guard lock(database_.mutex);
    if (auto doc = findMemory(database_.db, key)) {
        notes.memory = std::move(doc->content);
        notes.memoryVersion = doc->version;
        return notes;
    }
    Transaction tx(database_.db);
    if (auto doc = findMemory(database_.db, key)) {
        notes.memory = std::move(doc->content);
        notes.memoryVersion = doc->version;
    } else {
        notes.memory = seedPrivateMemory(key, bot, secretStorage_.redactor(bot), now);
    }
    tx.commit();
    return notes;
}

SessionContext MemoryStorage::sessionContext(const SessionContextTarget& target, const SoulLoader& soul, const SecretRedactor& scrub,
                                             std::size_t bytes, Seconds now) {
    if (bytes == 0) return {};
    const auto bot = target.bot;
    const auto role = soul ? scrub.text(soul(bot)) : std::string{};
    const auto key = privateKey(bot);
    const auto doc = findMemory(database_.db, key);
    const auto version = doc ? doc->version : Id{0};
    const auto memory = doc ? scrub.text(doc->content) : seedPrivateMemory(key, bot, scrub, now);
    const auto prior = lastSessionContext(database_, target);
    const auto name = bounded(scrub.text(target.displayName.empty() ? bot : target.displayName), 128);
    SessionContext context;
    context.role =
        sessionExcerpt(roleHash(role), role, {"Standing rules from " + name + "'s role:\n", "No standing rules.", "\n[Bot role truncated]"},
                       prior.role, std::min(maxBotRoleBytes, bytes));
    if (context.role) bytes -= context.role->section.size();
    context.memory =
        sessionExcerpt(version, memory, {"Notes from " + name + "'s memory:\n", "No memory notes.", "\n[Bot memory truncated]"},
                       prior.memory, std::min(limits::memory::maxDocumentBytes, bytes));
    return context;
}

void MemoryStorage::createMemory(std::string_view key, std::string_view bot, std::string_view id, std::string_view content, Seconds now) {
    database_.exec(R"SQL(
INSERT INTO memory_documents(doc_key,scope_kind,owner_bot,doc_id,content,version,updated_at)
VALUES(?,?,?,?,?,0,?)
)SQL",
                   {key, bot.empty() ? "shared" : "private", bot.empty() ? Json(nullptr) : Json(bot), id, content, now});
}

std::string MemoryStorage::seedPrivateMemory(std::string_view key, std::string_view bot, const SecretRedactor& scrub, Seconds now) {
    auto content = seed_ ? bounded(scrub.text(seed_(bot)), limits::memory::maxDocumentBytes) : std::string{};
    createMemory(key, bot, "memory", content, now);
    return content;
}

Json Memory::memory(const Person& actor, std::string_view bot, std::string_view operation, const Json& args) {
    std::lock_guard lock(database_.mutex);
    Transaction tx(database_.db);
    auto result = memory_.memory(bot, actor, operation, args, unixTime());
    tx.commit();
    return result;
}

Json MemoryStorage::memory(std::string_view bot, const Person& actor, std::string_view operation, const Json& args, Seconds now) {
    return accessMemory(bot, actor, operation, args, now, 0);
}

Json MemoryStorage::conversationMemory(const conversation::Message& message, std::string_view operation, const Json& args, Seconds now) {
    if ((operation == "write_memory" || operation == "revert_memory_edit") && !canEditFrom(message))
        throw AccessDenied("Memory access denied");
    const auto actor = conversationTurns.turnActor(message);
    return accessMemory(message.bot, actor, operation, args, now, message.id);
}

Json MemoryStorage::accessMemory(std::string_view bot, const Person& actor, std::string_view operation, const Json& args, Seconds now,
                                 Id sourceMessage) {
    const bool fromBot = sourceMessage != 0;
    const bool reading = operation == "read_memory" || operation == "memory_history";
    const bool writing = operation == "write_memory";
    const bool reverting = operation == "revert_memory_edit";
    if (!reading && !writing && !reverting) throw Conflict("Unknown memory operation");
    if (!validBotId(bot) || !args.is_object()) throw Conflict("Invalid memory arguments");
    const bool shared = args.contains("docId");
    const auto id = shared ? field(args, "docId") : "memory";
    if (!permissions.allows(actor, reading ? Action::bot_view : Action::bot_settings, bot)) throw AccessDenied("Memory access denied");
    if (shared) {
        const auto action = reading ? Action::shared_memory_read : Action::shared_memory_write;
        const bool botDenied = fromBot || reverting ? permissions.forbidden(bot, action, id) : !permissions.allows(bot, action, id);
        if (botDenied || !permissions.allows(actor, action, id)) throw AccessDenied("Memory access denied");
    }

    const auto key = shared ? "shared:" + id : privateKey(bot);
    const auto author = fromBot ? "bot:" + std::string(bot) : "person:" + actor.id;
    const auto scrub = shared ? secretStorage_.sharedRedactor() : secretStorage_.redactor(bot);
    scrub.requirePublic({key, author});
    auto doc = findMemory(database_.db, key);
    if (!doc && !shared) doc = MemoryDocument{seedPrivateMemory(key, bot, scrub, now), 0, now};
    const auto empty = MemoryDocument{};
    const auto& current = doc ? *doc : empty;
    const auto& [content, version, updatedAt] = current;

    if (operation == "read_memory")
        return scrub.record({{"docKey", key}, {"content", content}, {"version", version}, {"updatedAt", updatedAt}});
    if (operation == "memory_history") {
        const auto revision = args.contains("revisionId") ? requiredInteger(args, "revisionId") : 0;
        const auto before = !revision && args.contains("before") ? requiredInteger(args, "before") : 0;
        auto revisions = database_.query(R"SQL(
SELECT r.id,r.version,r.base_version,r.author,r.diff_summary,r.created_at,r.reverted_from,r.source,
       learned.data,json_extract(completed.data,'$.source'),
       CASE WHEN ?2<>0 THEN r.previous_content END,CASE WHEN ?2<>0 THEN r.new_content END
FROM memory_revisions r
LEFT JOIN journal learned ON learned.task_id=json_extract(r.source,'$.taskId') AND learned.kind='memory_learned'
  AND json_extract(learned.data,'$.revisionId')=r.id
LEFT JOIN journal completed ON completed.id=json_extract(learned.data,'$.completionEventId')
  AND completed.task_id=learned.task_id AND completed.kind='completed'
WHERE r.doc_key=?1 AND (?2=0 OR r.id=?2) AND (?3=0 OR r.id<?3)
ORDER BY r.id DESC LIMIT 21
)SQL",
                                         {key, revision, before});
        if (revision) {
            if (!revisions.next()) throw Conflict("Memory revision not found");
            auto detail = revisionView(revisions);
            detail["previousContent"] = revisions.string(10);
            detail["newContent"] = revisions.string(11);
            return scrub.record(std::move(detail));
        }
        Json history = Json::array();
        Id next = 0;
        while (revisions.next()) {
            if (history.size() == 20) {
                next = history.back().at("id");
                break;
            }
            history.push_back(revisionView(revisions));
        }
        return scrub.record({{"docKey", key}, {"version", version}, {"revisions", history}, {"before", next}});
    }

    const auto expected = requiredInteger(args, "baseVersion", 0);
    checkVersion(expected, version);
    std::string updated;
    Id reverted = 0;
    if (reverting) {
        reverted = requiredInteger(args, "revisionId");
        auto row = database_.query("SELECT previous_content FROM memory_revisions WHERE doc_key=? AND id=?", {key, reverted});
        if (!row.next()) throw Conflict("Memory revision not found");
        updated = row.string(0);
    } else {
        if (!args.contains("content") || !args.at("content").is_string() ||
            args.at("content").get_ref<const std::string&>().size() > limits::memory::maxDocumentBytes)
            throw Conflict("Memory content must be text up to " + std::to_string(limits::memory::maxDocumentBytes) + " bytes");
        updated = field(args, "content");
    }

    updated = scrub.text(updated);
    if (updated.size() > limits::memory::maxDocumentBytes)
        throw Conflict("Redacted memory content exceeds " + std::to_string(limits::memory::maxDocumentBytes) + " bytes");
    std::string summary;
    if (writing) {
        summary = field(args, "summary");
        if (!conversation_storage::hasText(summary) || summary.size() > 240 || summary.find_first_of("\r\n") != summary.npos)
            throw Conflict("Memory summary must be one line up to 240 bytes");
        if (fromBot && updated == content) return {{"version", version}, {"unchanged", true}};
    }
    if (!doc) createMemory(key, {}, id, {}, now);
    auto result = writeMemoryRevision(database_, key, current, updated, author, now, reverted);
    if (writing && fromBot) {
        Json notice{{"text", scrub.text(summary)},
                    {"revisionId", result.at("revisionId")},
                    {"version", result.at("version")},
                    {"replyToMessageId", sourceMessage}};
        if (shared) notice["docId"] = id;
        database_.exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'memory_remembered',?,?)", {now, notice, bot});
    }
    return result;
}

bool MemoryStorage::canEditFrom(const conversation::Message& message) {
    using enum conversation::PrincipalSource;
    return (message.principalSource == trusted || message.principalSource == web) && !message.routing.contains("sessionWake") &&
           !message.routing.contains("sessionInvitation") &&
           permissions.allows(conversationTurns.turnActor(message), Action::bot_settings, message.bot);
}

bool MemoryStorage::canLearnFrom(const TaskData& task) {
    if (task.trigger || !task.sourceMessageId) return false;
    const auto message = conversationTurns.loadTurnMessage(*task.sourceMessageId);
    return message.bot == task.botId && canEditFrom(message) && permissions.allows(task.botId, Action::private_memory_write, task.botId);
}

void MemoryStorage::learnFromWork(const TaskData& task, Id completion, Id report, std::string_view runId, const Json& args, Seconds now) {
    const auto scrub = secretStorage_.redactor(task.botId);
    const auto key = privateKey(task.botId);
    const auto author = "bot:" + task.botId;
    if (scrub.containsProtectedText(key) || scrub.containsProtectedText(author)) return;
    auto doc = findMemory(database_.db, key);
    if (!doc) doc = MemoryDocument{seedPrivateMemory(key, task.botId, scrub, now), 0, now};
    if (scrub.containsProtectedText(doc->content)) return;
    auto learned = mergeTaskLearnings(doc->content, args, runId, scrub);
    if (learned.added.empty() || scrub.containsProtectedText(learned.content)) return;
    const Json source{{"taskId", task.id}, {"runId", runId}};
    auto notice = writeMemoryRevision(database_, key, *doc, learned.content, author, now, 0, source);
    notice.erase("docKey");
    notice.update(source);
    notice["taskTitle"] = bounded(scrub.text(std::string_view(task.request).substr(0, task.request.find('\n'))), 300);
    notice["requestEventId"] = task.sourceMessageId.value();
    notice["completionEventId"] = completion;
    notice["reportEventId"] = report;
    notice["added"] = std::move(learned.added);
    notice["removed"] = std::move(learned.removed);
    database_.writeJournal(task.id, JournalKind::memory_learned, notice, {}, task.botId);
}

void MemoryStorage::projectMemoryNotice(std::string_view bot, Json& data, const Person* viewer) const {
    const auto document = field(data, "docId");
    if (!document.empty() && viewer && !permissions.allows(*viewer, Action::shared_memory_read, document)) {
        data = {{"text", "Shared memory updated."}};
        return;
    }
    const auto key = document.empty() ? privateKey(bot) : "shared:" + document;
    auto state = database_.query(R"SQL(
SELECT d.version=?1, undo.id IS NOT NULL FROM memory_documents d
LEFT JOIN memory_revisions undo ON undo.doc_key=d.doc_key AND undo.version=?1+1 AND undo.reverted_from=?2
WHERE d.doc_key=?3
)SQL",
                                 {data.at("version"), data.at("revisionId"), key});
    if (!state.next()) return;
    data["canUndo"] = state.number(0) != 0;
    data["undone"] = state.number(1) != 0;
}
}
