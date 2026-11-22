#include "internal.hpp"
#include "flock/session_read.hpp"

#include <charconv>
#include <chrono>

namespace flockv0::t3detail {
namespace {
constexpr int excerptChars = 480;
constexpr int detailChars = 2730;

bool notFound(const Failure& error) {
    return error.kind == Failure::Kind::Auth || error.code == "thread_not_found" ||
        error.code == "item_not_found" || error.code == "attachment_not_found" ||
        (error.code == "invalid_request" && error.serverMessage == "The thread was not found.");
}

template<typename Read>
auto withSession(const std::filesystem::path& directory, const SessionTarget& target, Read read)
    -> decltype(read(std::declval<Client&>())) {
    try {
        Client client(readCredential(directory,target.environmentId,target.config));
        return read(client);
    } catch (const Failure& error) {
        if (notFound(error)) return {};
        throw;
    }
}

Id parsePosition(const std::string& cursor) {
    if (cursor.empty()) return -1;
    Id result = 0;
    const auto [end,error] = std::from_chars(cursor.data(),cursor.data()+cursor.size(),result);
    if (error != std::errc() || end != cursor.data()+cursor.size() || result < 0 || result > maxHistoryPosition)
        throw Failure(Failure::Kind::Settings,"Invalid Session event cursor");
    return result;
}

Json read(Client& client, const SessionTarget& target, std::string_view view, int chars, unsigned limit,
          Json filter = Json::object()) {
    filter.update(Json{{"threadId",target.threadId},{"view",view},{"limit",limit},
                       {"maxCharsPerItem",chars},{"runLimit",1}});
    auto page = client.call("t3_thread_read",std::move(filter));
    if (str(member(page,"thread"),"threadId") != target.threadId || array(page,"items").size() > limit)
        invalidResponse();
    return page;
}

std::optional<SessionAttachment> attachment(const Json& file, bool withContent) {
    SessionAttachment value{};
    value.id = str(file,"id");
    if (value.id.empty() || value.id.size() > 1024 || file.value("available",true) == false) return {};
    value.name = bounded(str(file,"name"),240);
    value.mimeType = bounded(str(file,"mimeType"),128);
    value.truncated = file.value("truncated",false);
    if (withContent && file.contains("content") && file.at("content").is_string()) {
        const auto& content = file.at("content").get_ref<const std::string&>();
        value.content = bounded(content,8192);
        value.truncated |= value.content->size() < content.size();
    }
    return value;
}

SessionEvent event(const Json& item, std::size_t maxBytes, bool detail) {
    SessionEvent result{};
    result.id = str(item,"itemId");
    result.time = str(item,"updatedAt");
    result.kind = str(item,"type");
    if (result.id.empty() || result.time.empty() || result.kind.empty()) invalidResponse();
    result.status = str(item,"status");
    result.title = str(item,"title");
    const auto text = str(item,"text");
    result.text = bounded(text,maxBytes);
    result.truncated = boolean(item,"textTruncated") || result.text.size() < text.size();
    if (detail && result.kind == "command_execution") {
        const auto end = result.text.find('\n');
        if (result.text.starts_with("$ ")) {
            result.command = result.text.substr(2,end == std::string::npos ? end : end-2);
            if (end != std::string::npos) result.output = result.text.substr(end+1);
        } else {
            result.output = result.text;
        }
    }
    if (detail && result.kind == "error") result.error = result.text;
    if (const auto files = item.find("attachments"); files != item.end() && files->is_array()) {
        for (const auto& file : *files) {
            if (auto value = attachment(file,false)) result.attachments.push_back(std::move(*value));
            if (result.attachments.size() == sessionPageLimit) {
                result.truncated |= &file != &files->back();
                break;
            }
        }
    }
    return result;
}

std::optional<Json> readItem(Client& client, const SessionTarget& target, std::string_view itemId, int chars) {
    auto page = read(client,target,"activity",chars,1,{{"itemId",itemId}});
    const auto& items = array(page,"items");
    if (items.empty() || !localItem(items.front(),target.threadId) || str(items.front(),"itemId") != itemId) return {};
    return items.front();
}

void advance(const Json& item, Id& after) {
    const auto next = position(member(item,"position"));
    if (next <= after) invalidResponse();
    after = next;
}

bool more(const Json& page, Id after, Id start) {
    if (!boolean(page,"hasMore")) return false;
    if (after <= start || position(member(page,"nextPosition")) != after) invalidResponse();
    return true;
}
}

std::optional<SessionMetadata> findSession(const std::filesystem::path& directory, const SessionTarget& target,
                                           std::string_view query) {
    return withSession(directory,target,[&](Client& client) -> std::optional<SessionMetadata> {
        const auto page = read(client,target,"messages",1,1);
        const auto& thread = member(page,"thread");
        SessionMetadata item{str(thread,"title"),str(thread,"summary"),{},str(thread,"projectId"),
                             str(thread,"status"),str(thread,"createdAt"),str(thread,"updatedAt"),{}};
        if (item.projectId.empty() || item.createdAt.empty() || item.updatedAt.empty() || item.status.empty())
            invalidResponse();
        item.project = item.projectId;
        try {
            const auto project = client.call("t3_project_read",{{"projectId",item.projectId}});
            if (str(project,"id") != item.projectId) invalidResponse();
            if (auto title = str(project,"title"); !title.empty()) item.project = std::move(title);
        } catch (const Failure& error) {
            if (error.kind != Failure::Kind::Auth && error.kind != Failure::Kind::Tool) throw;
        }
        if (item.summary.empty()) item.summary = item.title;
        if (query.empty() || session_read::contains(item.title,query) || session_read::contains(item.summary,query) ||
            session_read::contains(item.project,query)) return item;
        const auto response = client.call("t3_thread_search",{{"projectId",item.projectId},{"query",query},{"limit",30}});
        for (const auto& match : array(response,"matches")) {
            if (str(match,"threadId") != target.threadId || str(match,"projectId") != item.projectId) continue;
            item.matchExcerpt = bounded(str(match,"snippet"),480);
            return item;
        }
        return {};
    });
}

std::optional<SessionEventPage> sessionEvents(const std::filesystem::path& directory, const SessionTarget& target,
                                             const SessionEventFilter& filter) {
    return withSession(directory,target,[&](Client& client) -> std::optional<SessionEventPage> {
        if (filter.limit == 0 || filter.limit > sessionPageLimit)
            throw Failure(Failure::Kind::Settings,"Invalid Session event page size");
        auto after = parsePosition(filter.cursor);
        SessionEventPage result;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(12);
        for (int scan = 0; scan < 12; ++scan) {
            const auto start = after;
            const auto page = read(client,target,"activity",filter.query.empty() ? excerptChars : detailChars,
                                   filter.limit-static_cast<unsigned>(result.items.size()),{{"afterPosition",after}});
            for (const auto& item : array(page,"items")) {
                advance(item,after);
                if (!localItem(item,target.threadId) || (!filter.kind.empty() && str(item,"type") != filter.kind) ||
                    !session_read::inRange(str(item,"updatedAt"),filter.fromTime,filter.toTime)) continue;
                if (!session_read::contains(str(item,"text"),filter.query) &&
                    !session_read::contains(str(item,"title"),filter.query)) {
                    result.truncated |= boolean(item,"textTruncated");
                    continue;
                }
                result.items.push_back(event(item,480,false));
            }
            if (!more(page,after,start)) return result;
            if (result.items.size() == filter.limit || std::chrono::steady_clock::now() >= deadline) break;
        }
        result.nextCursor = std::to_string(after);
        return result;
    });
}

std::optional<SessionEvent> sessionEvent(const std::filesystem::path& directory, const SessionTarget& target,
                                       std::string_view itemId) {
    return withSession(directory,target,[&](Client& client) -> std::optional<SessionEvent> {
        const auto item = readItem(client,target,itemId,detailChars);
        if (!item) return {};
        return event(*item,8192,true);
    });
}

std::optional<SessionCounts> sessionCounts(const std::filesystem::path& directory, const SessionTarget& target) {
    return withSession(directory,target,[&](Client& client) -> std::optional<SessionCounts> {
        SessionCounts result;
        Id after = -1;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
        for (;;) {
            const auto start = after;
            const auto page = read(client,target,"messages",1,100,{{"afterPosition",after}});
            if (start == -1) result.runs = position(member(member(page,"thread"),"runCount"));
            for (const auto& item : array(page,"items")) {
                advance(item,after);
                const auto kind = str(item,"type");
                if (localItem(item,target.threadId) && (kind == "user_message" || kind == "assistant_message")) ++result.messages;
            }
            if (!more(page,after,start)) return result;
            if (std::chrono::steady_clock::now() >= deadline)
                throw Failure(Failure::Kind::Wire,"T3 Session count did not finish");
        }
    });
}

std::optional<SessionAttachment> sessionAttachment(const std::filesystem::path& directory, const SessionTarget& target,
                                                   std::string_view itemId, std::string_view attachmentId) {
    return withSession(directory,target,[&](Client& client) -> std::optional<SessionAttachment> {
        const auto item = readItem(client,target,itemId,1);
        if (!item) return {};
        const auto files = item->find("attachments");
        if (files == item->end() || !files->is_array()) return {};
        for (const auto& file : *files)
            if (str(file,"id") == attachmentId) return attachment(file,true);
        return {};
    });
}
}
