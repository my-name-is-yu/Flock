#include "storage.hpp"
#include "flock/session_read.hpp"

#include <charconv>
#include <chrono>

namespace flockv0 {
namespace {
constexpr auto unavailable = "Session evidence not found";

Json failure(std::string_view error) { return {{"ok",false},{"error",error}}; }

std::string argument(const Json& args, const char* key, std::size_t max = 1024) {
    const auto value = args.find(key);
    if (value == args.end() || value->is_null()) return {};
    if (!value->is_string() || value->get_ref<const std::string&>().size() > max)
        throw Conflict(std::string("Invalid ") + key);
    return value->get<std::string>();
}

unsigned pageSize(const Json& args) {
    const auto limit = args.find("limit");
    if (limit == args.end() || limit->is_null()) return 10;
    if (!limit->is_number_integer() || *limit < 1 || *limit > sessionPageLimit)
        throw Conflict("Invalid limit");
    return limit->get<unsigned>();
}

bool utc(std::string_view value) {
    if (value.empty()) return true;
    if (value.size() < 20 || value.size() > 30 || value.back() != 'Z' ||
        value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':') return false;
    if (value.size() != 20 && (value.size() < 22 || value[19] != '.' ||
        value.substr(20,value.size()-21).find_first_not_of("0123456789") != value.npos)) return false;
    const auto read = [&](std::size_t start, std::size_t length) {
        const auto part = value.substr(start,length);
        if (part.find_first_not_of("0123456789") != value.npos) return -1;
        int number = 0;
        std::from_chars(part.data(),part.data()+part.size(),number);
        return number;
    };
    using namespace std::chrono;
    const auto yearNumber = read(0,4);
    return yearNumber >= 0 && (year{yearNumber}/read(5,2)/read(8,2)).ok() &&
        read(11,2) >= 0 && read(11,2) <= 23 && read(14,2) >= 0 && read(14,2) <= 59 &&
        read(17,2) >= 0 && read(17,2) <= 59;
}

struct Reference {
    SessionTarget target;
    Json payload;
};

Scope scope(const SessionTarget& target) { return Scope::sessionRead(target.environmentId,target.threadId); }

Json nextCursor(const std::string& cursor) {
    return cursor.empty() ? Json(nullptr) : Json(cursor);
}

Json eventView(const SessionEvent& event, bool detail) {
    const auto maxText = detail ? 8192U : 480U;
    auto text = bounded(event.text,maxText);
    auto title = bounded(event.title,240);
    bool truncated = event.truncated || text.size() < event.text.size() || title.size() < event.title.size();
    Json result{{"timestamp",event.time},{"kind",bounded(event.kind,128)},
                {"status",bounded(event.status,128)},{"title",std::move(title)},
                {detail ? "text" : "excerpt",std::move(text)}};
    if (detail) {
        for (const auto& [key,value] : {std::pair{"command",&event.command},
                                       {"output",&event.output},{"error",&event.error}}) {
            if (value->empty()) continue;
            auto content = bounded(*value,maxText);
            truncated |= content.size() < value->size();
            result[key] = std::move(content);
        }
    }
    result["truncated"] = truncated;
    return result;
}
}

void Store::Impl::migrateSessionRead() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS session_read_refs(
 ref TEXT PRIMARY KEY, bot TEXT NOT NULL, kind TEXT NOT NULL,
 environment TEXT NOT NULL, thread TEXT NOT NULL, payload TEXT NOT NULL,
 UNIQUE(bot,kind,environment,thread,payload))
)SQL");
}

Json Store::Impl::sessionReadTool(std::string_view bot, const Json& config, const Decision& decision, T3& t3) {
    Access access(*this);
    const auto permitted = [&](const SessionTarget& target) { return access.permits(bot,scope(target),true); };
    const auto resolve = [&](std::string_view ref, std::string_view kind) -> std::optional<Reference> {
        std::lock_guard lock(mutex);
        auto row = query("SELECT environment,thread,payload FROM session_read_refs WHERE ref=? AND bot=? AND kind=?",
                         {ref,bot,kind});
        if (!row.next()) return {};
        return Reference{{row.string(0),row.string(1),config},row.json(2)};
    };
    const auto issue = [&](std::string_view kind, const SessionTarget& target, const Json& payload) {
        const auto key = Json::array({bot,kind,target.environmentId,target.threadId,payload.dump()});
        auto values = key;
        values.push_back(actionId());
        exec("INSERT INTO session_read_refs(bot,kind,environment,thread,payload,ref) VALUES(?,?,?,?,?,?)"
             " ON CONFLICT(bot,kind,environment,thread,payload) DO NOTHING",values);
        auto row = query("SELECT ref FROM session_read_refs WHERE bot=? AND kind=? AND environment=? AND thread=? AND payload=?",key);
        if (!row.next()) throw std::logic_error("Missing Session reference");
        return row.string(0);
    };
    try {
        const auto& args = decision.args;
        if (!args.is_object()) throw Conflict("Tool arguments must be an object");
        const auto queryText = argument(args,"query");
        const auto from = argument(args,"fromTime",30);
        const auto to = argument(args,"toTime",30);
        if (!utc(from) || !utc(to) || (!from.empty() && !session_read::inRange(from,{},to)))
            throw Conflict("Use an ordered UTC date-time range");
        const auto limit = pageSize(args);
        const auto cursor = argument(args,"cursor",128);

        if (decision.tool == "flock_session_search") {
            const auto project = argument(args,"project");
            const auto status = argument(args,"status",128);
            const auto filters = Json::array({queryText,project,status,from,to});
            SessionTarget after;
            if (!cursor.empty()) {
                const auto anchor = resolve(cursor,"search");
                if (!anchor || anchor->payload != filters || !permitted(anchor->target) ||
                    !t3.findSession(anchor->target,{})) return failure(unavailable);
                after = anchor->target;
            }
            std::vector<SessionTarget> candidates;
            {
                std::lock_guard lock(mutex);
                auto rows = query(R"SQL(
WITH candidates(environment,thread) AS (
 SELECT r.environment_id,r.thread_id FROM runs r JOIN tasks t ON t.id=r.task_id WHERE t.bot_id=?1
 UNION
 SELECT json_extract(resource,'$[0]'),json_extract(resource,'$[1]') FROM bot_grants
  WHERE bot=?1 AND kind='session' AND op='read'
)
SELECT environment,thread FROM candidates WHERE (environment,thread) > (?2,?3) ORDER BY environment,thread
)SQL",{bot,after.environmentId,after.threadId});
                while (rows.next()) candidates.push_back({rows.string(0),rows.string(1),config});
            }
            std::vector<std::pair<SessionTarget,SessionMetadata>> matches;
            for (const auto& target : candidates) {
                if (!permitted(target)) continue;
                auto item = t3.findSession(target,queryText);
                if (!item || !permitted(target) ||
                    (!project.empty() && item->projectId != project && !session_read::contains(item->project,project)) ||
                    (!status.empty() && item->status != status) ||
                    !session_read::inRange(item->updatedAt,from,to)) continue;
                matches.emplace_back(target,std::move(*item));
                if (matches.size() > limit) break;
            }
            std::lock_guard lock(mutex);
            Transaction transaction(db);
            if (!cursor.empty() && !permitted(after)) return failure(unavailable);
            auto items = Json::array();
            std::string next;
            SessionTarget last;
            for (const auto& [target,item] : matches) {
                if (!permitted(target)) continue;
                if (items.size() == limit) {
                    next = issue("search",last,filters);
                    break;
                }
                items.push_back({{"sessionRef",issue("session",target,nullptr)},{"title",bounded(item.title,240)},
                    {"summary",bounded(item.summary,480)},{"project",bounded(item.project,240)},
                    {"status",bounded(item.status,128)},{"createdAt",item.createdAt},{"updatedAt",item.updatedAt},
                    {"matchExcerpt",bounded(item.matchExcerpt,480)}});
                last = target;
            }
            transaction.commit();
            return redactor(bot).record({{"ok",true},{"sessions",std::move(items)},{"nextCursor",nextCursor(next)}},
                                        SecretRedactor::Fields::external);
        }

        const auto session = resolve(argument(args,"sessionRef",128),"session");
        if (!session || !permitted(session->target)) return failure(unavailable);
        const auto& target = session->target;
        const auto operation = argument(args,"operation",32);
        const auto scoped = [&](std::string_view ref, std::string_view kind) -> std::optional<Json> {
            const auto resolved = resolve(ref,kind);
            if (!resolved || scope(target).resource != scope(resolved->target).resource) return {};
            return resolved->payload;
        };
        const auto list = operation == "list" || operation == "search";
        const auto kind = argument(args,"kind",128);
        const auto filters = Json::array({operation,queryText,kind,from,to});
        std::optional<SessionEventPage> page;
        std::optional<SessionEvent> event;
        std::optional<SessionAttachment> attachment;
        std::optional<SessionCounts> counts;
        std::string attachmentRef;
        if (list) {
            std::string position;
            if (!cursor.empty()) {
                auto saved = scoped(cursor,"events");
                if (!saved || (*saved)[0] != filters) return failure(unavailable);
                position = (*saved)[1].get<std::string>();
            }
            page = t3.sessionEvents(target,{queryText,kind,from,to,limit,position});
        } else if (operation == "read_event") {
            auto saved = scoped(argument(args,"eventRef",128),"event");
            if (!saved) return failure(unavailable);
            event = t3.sessionEvent(target,saved->get<std::string>());
        } else if (operation == "counts") {
            counts = t3.sessionCounts(target);
        } else if (operation == "get_attachment") {
            attachmentRef = argument(args,"attachmentRef",128);
            auto saved = scoped(attachmentRef,"attachment");
            if (!saved) return failure(unavailable);
            attachment = t3.sessionAttachment(target,(*saved)[0].get<std::string>(),(*saved)[1].get<std::string>());
        } else {
            throw Conflict("Invalid Session event operation");
        }

        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (!permitted(target) || (!page && !event && !attachment && !counts)) return failure(unavailable);
        Json result{{"ok",true},{"truncated",false}};
        const auto format = [&](const SessionEvent& item, bool detail) {
            auto view = eventView(item,detail);
            view["eventRef"] = issue("event",target,item.id);
            auto& attachments = view["attachments"] = Json::array();
            for (const auto& file : item.attachments) {
                if (attachments.size() == sessionPageLimit) {
                    view["truncated"] = true;
                    break;
                }
                if (file.id.empty()) continue;
                attachments.push_back({{"attachmentRef",issue("attachment",target,Json::array({item.id,file.id}))},
                    {"name",bounded(file.name,240)},{"mimeType",bounded(file.mimeType,128)}});
            }
            return view;
        };
        if (page) {
            if (page->items.size() > limit) throw std::logic_error("Invalid Session event page");
            auto& items = result["events"] = Json::array();
            for (const auto& item : page->items) items.push_back(format(item,false));
            result["nextCursor"] = page->nextCursor.empty() ? Json(nullptr) :
                Json(issue("events",target,Json::array({filters,page->nextCursor})));
            result["truncated"] = page->truncated || std::ranges::any_of(items,[](const Json& item) {
                return item.at("truncated") == true;
            });
        } else if (event) {
            result["event"] = format(*event,true);
            result["truncated"] = result["event"]["truncated"];
        } else if (counts) {
            result["counts"] = {{"messages",counts->messages},{"runs",counts->runs}};
        } else {
            Json view{{"attachmentRef",attachmentRef},{"name",bounded(attachment->name,240)},
                      {"mimeType",bounded(attachment->mimeType,128)}};
            bool truncated = attachment->truncated;
            if (attachment->content) {
                auto content = bounded(*attachment->content,8192);
                truncated |= content.size() < attachment->content->size();
                view["content"] = std::move(content);
            }
            result["attachment"] = std::move(view);
            result["truncated"] = truncated;
        }
        transaction.commit();
        return redactor(bot).record(std::move(result),SecretRedactor::Fields::external);
    } catch (const Conflict& error) {
        return failure(error.what());
    } catch (const std::exception&) {
        return failure("Session evidence could not be read");
    }
}
}
