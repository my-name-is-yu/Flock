#include "conversation_storage.hpp"

namespace flockv0 {
void Store::Impl::migrateCompaction() {
    exec(R"SQL(CREATE TABLE IF NOT EXISTS conversation_compactions(
        id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, start_message_id INTEGER NOT NULL REFERENCES journal(id),
        end_message_id INTEGER NOT NULL REFERENCES journal(id), summary TEXT NOT NULL, original_count INTEGER NOT NULL,
        created_at INTEGER NOT NULL, created_by TEXT NOT NULL))SQL");
    exec("CREATE INDEX IF NOT EXISTS compactions_bot ON conversation_compactions(bot_id,id DESC)");
    exec(R"SQL(CREATE VIEW IF NOT EXISTS compactable_messages AS
        SELECT j.id,coalesce(j.conversation_bot_id,t.bot_id) bot_id,j.kind,
          coalesce(m.text,json_extract(j.data,'$.report'),json_extract(j.data,'$.text'),'') text
        FROM journal j LEFT JOIN messages m ON m.id=j.id LEFT JOIN tasks t ON t.id=j.task_id
        WHERE j.kind IN ('conversation_message','conversation_reply','completed','cancelled','report')
          OR (j.kind='waiting' AND json_extract(j.data,'$.reason')='user_question'))SQL");
}

Json Store::Impl::compactions(std::string_view bot, Id before, unsigned limit) const {
    auto rows = query("SELECT id,start_message_id,end_message_id,summary,original_count,created_at,created_by "
                      "FROM conversation_compactions WHERE bot_id=? AND (?=0 OR id<?) ORDER BY id DESC LIMIT ?",
                      {bot,before,before,std::min(limit,100u)});
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

Json Store::Impl::contextTool(std::string_view tool, const Json& args, std::string_view bot,
                              Id message, int step, const Json& routing) {
    if (tool == "continue_fresh_session") {
        auto task = ownTask(args.at("taskId").get<Id>(),bot);
        if (terminal(task) || task.at("state") == "cancelling") throw Conflict("Task is closed or stopping");
        auto instruction = args.value("instruction",std::string("Continue with the same completion criteria."));
        if (!conversation_storage::hasText(instruction) || instruction.size()>8000) throw Conflict("Invalid instruction");
        task["freshRequested"] = true;
        task["replanning"] = true;
        save(task);
        acceptInstruction(task.at("id"),task.value("currentRevision",Id{}),message,step,instruction,nullptr,routing,nullptr);
        return {{"ok",true},{"freshContext",true},{"taskId",task.at("id")},{"effect","replanning"}};
    }

    const auto latest = compactions(bot,0,1);
    const Id boundary = latest.empty() ? 0 : latest.front().at("endMessageId").get<Id>();
    if (tool == "compact_conversation") {
        const auto end = args.at("endMessageId").get<Id>();
        const auto summary = field(args,"summary");
        if (!conversation_storage::hasText(summary) || summary.size()>4000 || end<boundary || end>=message)
            throw Conflict("Supply a cumulative summary up to 4000 bytes and an older message boundary");
        auto range = query("SELECT min(id),max(id),count(*) FROM compactable_messages WHERE bot_id=? AND id<=?",{bot,end});
        range.next();
        if (!end || range.number(1)!=end) throw Conflict("Unknown conversation boundary");
        exec("INSERT INTO conversation_compactions(bot_id,start_message_id,end_message_id,summary,original_count,"
             "created_at,created_by) VALUES(?,?,?,?,?,?,?)",{bot,range.number(0),end,summary,range.number(2),unixTime(),bot});
        return {{"ok",true},{"compaction",compactions(bot,0,1).front()}};
    }

    Id end = boundary;
    if (args.contains("compactionId")) {
        auto row = query("SELECT end_message_id FROM conversation_compactions WHERE bot_id=? AND id=?",{bot,args.at("compactionId")});
        if (!row.next()) throw Conflict("Unknown compaction");
        end = row.number(0);
    }
    end = std::min(end,args.value("endMessageId",end));
    const auto start = args.value("startMessageId",Id{1});
    const auto needle = field(args,"query");
    if (needle.size()>512 || start<=0 || end<start) throw Conflict("Invalid compacted message range or query");
    auto rows = query("SELECT id,kind,text FROM compactable_messages WHERE bot_id=? AND id BETWEEN ? AND ? "
                      "AND instr(lower(text),lower(?))>0 ORDER BY id LIMIT 9",{bot,start,end,needle});
    Json result = {{"ok",true},{"messages",Json::array()}};
    const auto limit = std::clamp(args.value("limit",8),1,8);
    auto offset = args.value("offset",std::size_t{0});
    while (rows.next()) {
        if (result["messages"].size()>=static_cast<unsigned>(limit) || result.dump().size()>5900) {
            result["nextMessageId"] = rows.number(0);
            break;
        }
        auto body = rows.string(2);
        if (offset>body.size() || (offset<body.size() && (static_cast<unsigned char>(body[offset])&0xc0)==0x80))
            throw Conflict("Invalid UTF-8 byte offset");
        auto chunk = bounded(std::string_view(body).substr(offset),std::min<std::size_t>(2000,(7900-result.dump().size())/6));
        result["messages"].push_back({{"messageId",rows.number(0)},{"kind",rows.string(1)},{"text",chunk}});
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
