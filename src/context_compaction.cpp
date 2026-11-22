#include "conversation_storage.hpp"

namespace flockv0 {
using conversation_storage::identifier;
using conversation_storage::expandedContextBytes;

namespace {
std::string boundedJsonText(std::string_view text, std::size_t limit, std::size_t bytes) {
    auto prefix = bounded(text,std::min(limit,bytes));
    if (Json(prefix).dump().size()-2 <= bytes) return prefix;

    std::size_t low = 0, high = prefix.size()-1;
    while (low < high) {
        const auto middle = low + (high-low+1)/2;
        if (Json(bounded(text,middle)).dump().size()-2 <= bytes) low = middle;
        else high = middle-1;
    }
    return bounded(text,low);
}
}

void Store::Impl::initializeCompactionSchema() {
    Transaction tx(db);
    exec(R"SQL(CREATE TABLE IF NOT EXISTS conversation_compactions(
        id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, start_message_id INTEGER NOT NULL REFERENCES journal(id),
        end_message_id INTEGER NOT NULL REFERENCES journal(id), summary TEXT NOT NULL, original_count INTEGER NOT NULL,
        created_at INTEGER NOT NULL, created_by TEXT NOT NULL))SQL");
    exec("CREATE INDEX IF NOT EXISTS compactions_bot ON conversation_compactions(bot_id,id DESC)");
    exec(R"SQL(CREATE VIEW IF NOT EXISTS compactable_messages AS
        SELECT j.id,coalesce(j.conversation_bot_id,t.bot_id) bot_id,j.kind,j.task_id,j.data,
          coalesce(m.text,json_extract(j.data,'$.report'),json_extract(j.data,'$.text'),'') text
        FROM journal j LEFT JOIN messages m ON m.id=j.id LEFT JOIN tasks t ON t.id=j.task_id
        WHERE j.kind IN ('conversation_message','conversation_reply','completed','cancelled','report')
          OR (j.kind='waiting' AND json_extract(j.data,'$.reason')='user_question')
          OR (j.kind='user_answer' AND j.conversation_bot_id IS NULL AND json_extract(j.data,'$.conversation') IS NOT 1))SQL");
    tx.commit();
}

Json Store::Impl::compactions(std::string_view bot, Id before, unsigned limit, Id beforeMessage) const {
    auto rows = query("SELECT id,start_message_id,end_message_id,summary,original_count,created_at,created_by "
                      "FROM conversation_compactions WHERE bot_id=? AND (?=0 OR id<?) AND (?=0 OR end_message_id<?) ORDER BY id DESC LIMIT ?",
                      {bot,before,before,beforeMessage,beforeMessage,std::min(limit,100u)});
    Json result = Json::array();
    while (rows.next()) result.push_back({{"compactionId",rows.number(0)},{"startMessageId",rows.number(1)},
        {"endMessageId",rows.number(2)},{"summary",rows.string(3)},{"originalCount",rows.number(4)},
        {"createdAt",rows.number(5)},{"createdBy",rows.string(6)}});
    return result;
}

Json Store::compactions(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    return p_->compactions(bot,before,limit);
}

Json Store::Impl::conversationHistory(std::string_view bot, Id before, const Json& summaries, std::size_t expandedBytes) const {
    std::size_t remaining = 24000 - expandedBytes - 2;
    Json history = Json::array();
    Id boundary = 0;
    Json summary;
    if (!summaries.empty()) {
        const auto& saved = summaries.front();
        boundary = saved.at("endMessageId");
        summary = {{"kind","conversation_compaction"},{"compactionId",saved.at("compactionId")},
                   {"startMessageId",saved.at("startMessageId")},{"endMessageId",boundary},{"summary",saved.at("summary")}};
        remaining -= summary.dump().size();
    }
    auto rows = query(R"SQL(
SELECT id,task_id,kind,data,text FROM compactable_messages WHERE bot_id=? AND id>? AND (
    (kind='conversation_message' AND id<?) OR
    (kind='conversation_reply' AND json_extract(data,'$.replyToMessageId')<?) OR
    kind NOT IN ('conversation_message','conversation_reply'))
ORDER BY id DESC LIMIT 24
)SQL",{bot,boundary,before,before});
    while (rows.next()) {
        const auto kind = rows.string(2);
        const bool user = kind=="conversation_message" || kind=="user_answer";
        const bool reply = kind=="conversation_reply";
        const char* role = user ? "user" : reply ? "bot" : "task";
        Json entry = {{"messageId",rows.number(0)},{"kind",kind},{"role",role},{"text",""}};
        if (rows.number(1)) entry["taskId"] = rows.number(1);
        if (kind=="conversation_reply") {
            const auto data = rows.json(3);
            for (const auto* key : {"actions","suggestions"})
                if (data.contains(key)) entry[key] = data.at(key);
        } else if (kind=="waiting") entry["waitId"] = rows.json(3).at("id");
        else if (kind=="user_answer") entry["waitId"] = conversation_storage::integer(rows.json(3),"waitId");
        const std::size_t limit = user ? 4000 : reply ? 1500 : 700;
        const auto bytes = entry.dump().size() + (history.empty() && summary.is_null() ? 0 : 1);
        if (bytes>remaining) break;
        entry["text"] = boundedJsonText(rows.string(4),limit,remaining-bytes);
        remaining -= bytes + (entry.at("text").dump().size()-2);
        history.push_back(std::move(entry));
    }
    if (!summary.is_null()) history.push_back(std::move(summary));
    std::reverse(history.begin(),history.end());
    return history;
}

Json Store::Impl::contextTool(std::string_view tool, const Json& args, std::string_view bot,
                              Id message, int step, const Json& routing) {
    if (tool == "continue_fresh_session") {
        auto task = ownTask(identifier(args,"taskId",true),bot);
        if (terminal(task) || task.at("state") == "cancelling") throw Conflict("Task is closed or stopping");
        const auto instruction = args.contains("instruction") ? conversation_storage::text(args,"instruction",8000,true)
                                                              : "Continue with the same completion criteria.";
        task["newSessionRequired"] = true;
        task["freshContextRequested"] = true;
        task["replanning"] = true;
        acceptInstruction(task,message,step,instruction,nullptr,routing,nullptr);
        return {{"ok",true},{"freshContext",true},{"taskId",task.at("id")},{"effect","replanning"}};
    }

    const auto latest = compactions(bot,0,1);
    const Id boundary = latest.empty() ? 0 : latest.front().at("endMessageId").get<Id>();
    if (tool == "compact_conversation") {
        const auto end = identifier(args,"endMessageId",true);
        const auto summary = conversation_storage::text(args,"summary",4000,true);
        if (Json(summary).dump().size()>expandedContextBytes) throw Conflict("Escaped summary exceeds the context budget");
        if (end<boundary || end>=message) throw Conflict("Supply an older boundary covering the previous summary");
        auto range = query("SELECT min(id),max(id),count(*) FROM compactable_messages WHERE bot_id=? AND id<=?",{bot,end});
        range.next();
        if (range.number(1)!=end) throw Conflict("Unknown conversation boundary");
        exec("INSERT INTO conversation_compactions(bot_id,start_message_id,end_message_id,summary,original_count,"
             "created_at,created_by) VALUES(?,?,?,?,?,?,?)",{bot,range.number(0),end,summary,range.number(2),unixTime(),bot});
        return {{"ok",true},{"compaction",compactions(bot,0,1).front()}};
    }

    Id end = std::min(boundary,message-1);
    if (args.contains("compactionId")) {
        auto row = query("SELECT end_message_id FROM conversation_compactions WHERE bot_id=? AND id=?",{bot,identifier(args,"compactionId",true)});
        if (!row.next()) throw Conflict("Unknown compaction");
        end = std::min(end,row.number(0));
    }
    if (args.contains("endMessageId")) end = std::min(end,identifier(args,"endMessageId",true));
    const Id start = args.contains("startMessageId") ? identifier(args,"startMessageId",true) : 1;
    const auto needle = conversation_storage::text(args,"query",512,false);
    if (end<start) throw Conflict("No compacted messages in this range");
    auto rows = query("SELECT id,kind,text FROM compactable_messages WHERE bot_id=? AND id BETWEEN ? AND ? "
                      "AND instr(lower(text),lower(?))>0 ORDER BY id LIMIT 9",{bot,start,end,needle});
    Json result = {{"ok",true},{"messages",Json::array()}};
    constexpr std::size_t chunkBytes = 2000, cursorBytes = 128;
    const std::size_t limit = args.contains("limit") ? std::min<Id>(identifier(args,"limit",true),8) : 8;
    std::size_t offset = identifier(args,"offset",false,0);
    if (offset && !args.contains("startMessageId")) throw Conflict("Byte cursor needs its message ID");
    std::size_t used = result.dump().size()+cursorBytes;
    while (rows.next()) {
        if (result["messages"].size()>=limit || expandedContextBytes-used<cursorBytes) {
            result["nextMessageId"] = rows.number(0);
            break;
        }
        auto body = rows.string(2);
        if ((offset && rows.number(0)!=start) || offset>body.size() || (offset<body.size() && (static_cast<unsigned char>(body[offset])&0xc0)==0x80))
            throw Conflict("Invalid UTF-8 byte offset");
        Json entry = {{"messageId",rows.number(0)},{"kind",rows.string(1)},{"text",""}};
        const auto available = expandedContextBytes-used-entry.dump().size()-1;
        const auto tail = std::string_view(body).substr(offset);
        auto chunk = bounded(tail,chunkBytes);
        if (Json(chunk).dump().size()-2>available)
            chunk = bounded(tail,available/6);
        entry["text"] = chunk;
        used += entry.dump().size()+1;
        result["messages"].push_back(std::move(entry));
        if (offset+chunk.size()<body.size()) {
            result["nextMessageId"] = rows.number(0);
            result["nextOffset"] = offset+chunk.size();
            break;
        }
        offset = 0;
    }
    return result;
}
}
