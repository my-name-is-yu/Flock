#include "internal.hpp"
#include "flock/session_read.hpp"

#include <chrono>

namespace flockv0::t3detail {
namespace {
constexpr int excerptChars = 480;
constexpr int detailChars = 2730;
constexpr int eventScanPages = 12;
constexpr int countScanPages = 160;
using Clock = std::chrono::steady_clock;

template<class Read>
auto withSession(const std::filesystem::path& directory, const SessionTarget& target, Read read)
    -> decltype(read(std::declval<Client&>())) {
    try {
        Client client(loadCredential(directory,target.environmentId));
        return read(client);
    } catch (const Failure& error) {
        if (error.kind == Failure::Kind::Auth || error.kind == Failure::Kind::Settings ||
            error.reason == Failure::Reason::HistoryNotFound) return {};
        throw;
    }
}

std::optional<SessionAttachment> attachment(const Json& file) {
    const auto id = str(file,"id");
    if (id.empty() || id.size() > 1024 || !file.value("available",true)) return {};
    return SessionAttachment{id,str(file,"name"),str(file,"mimeType"),{},file.value("truncated",false)};
}

SessionEvent event(const Json& item) {
    SessionEvent result{};
    result.id = str(item,"itemId");
    result.time = str(item,"updatedAt");
    result.kind = str(item,"type");
    if (result.id.empty() || !session_read::utcTimestamp(result.time) || result.kind.empty()) invalidResponse();
    result.status = str(item,"status");
    result.title = str(item,"title");
    result.text = str(item,"text");
    result.textTruncated = boolean(item,"textTruncated");
    return result;
}

void attachMetadata(SessionEvent& result, const Json& item) {
    const auto files = item.find("attachments");
    if (files == item.end()) return;
    if (!files->is_array()) invalidResponse();
    for (const auto& file : *files) {
        auto value = attachment(file);
        if (!value) continue;
        if (result.attachments.size() == sessionPageLimit) {
            result.attachmentsTruncated = true;
            break;
        }
        result.attachments.push_back(std::move(*value));
    }
}

std::optional<Json> readLocalItem(Client& client, const SessionTarget& target, std::string_view itemId, int chars) {
    auto page = readThread(client,target.threadId,"activity",1,chars,{{"itemId",itemId}});
    const auto& items = array(page,"items");
    if (items.empty() || !localItem(items.front(),target.threadId) || str(items.front(),"itemId") != itemId) return {};
    return items.front();
}
}

std::optional<SessionMetadata> sessionMetadata(const std::filesystem::path& directory, const SessionTarget& target,
                                              bool includeMessages) {
    return withSession(directory,target,[&](Client& client) -> std::optional<SessionMetadata> {
        const auto page = readThread(client,target.threadId,"messages",includeMessages ? sessionPageLimit : 1,
                                     includeMessages ? 240 : 1,{{"beforePosition",maxHistoryPosition}});
        const auto& thread = member(page,"thread");
        SessionMetadata metadata{str(thread,"title"),{},str(thread,"projectId"),str(thread,"status"),
                                 str(thread,"createdAt"),str(thread,"updatedAt"),{},false};
        if (metadata.projectId.empty() || metadata.status.empty() ||
            !session_read::utcTimestamp(metadata.createdAt) || !session_read::utcTimestamp(metadata.updatedAt))
            invalidResponse();
        try {
            const auto project = client.call("t3_project_read",{{"projectId",metadata.projectId}});
            if (str(project,"id") != metadata.projectId) invalidResponse();
            metadata.project = str(project,"title");
        } catch (const Failure& error) {
            if (error.kind != Failure::Kind::Auth && error.reason != Failure::Reason::ProjectNotFound) throw;
        }
        if (includeMessages) {
            const auto& earlier = member(page,"earlierPosition");
            metadata.moreMessages = !earlier.is_null();
            if (metadata.moreMessages) position(earlier);
            for (const auto& item : array(page,"items")) {
                if (localItem(item,target.threadId))
                    metadata.messages.push_back({str(item,"text"),boolean(item,"textTruncated")});
            }
        }
        return metadata;
    });
}

std::optional<SessionEventPage> sessionEvents(const std::filesystem::path& directory, const SessionTarget& target,
                                             const SessionEventFilter& filter) {
    if (filter.limit == 0 || filter.limit > sessionPageLimit ||
        (filter.afterPosition && (*filter.afterPosition < 0 || *filter.afterPosition > maxHistoryPosition)))
        throw Failure(Failure::Kind::Settings,"Invalid Session event pagination");
    return withSession(directory,target,[&](Client& client) -> std::optional<SessionEventPage> {
        auto after = filter.afterPosition.value_or(-1);
        SessionEventPage result;
        const auto deadline = Clock::now()+std::chrono::seconds(12);
        for (int scan = 0; scan < eventScanPages; ++scan) {
            const auto start = after;
            const auto page = readThread(client,target.threadId,"activity",
                filter.limit-static_cast<unsigned>(result.items.size()),
                filter.matchText ? detailChars : excerptChars,{{"afterPosition",after}});
            for (const auto& item : array(page,"items")) {
                advancePosition(item,after);
                if (!localItem(item,target.threadId)) continue;
                auto evidence = event(item);
                if ((!filter.kind.empty() && evidence.kind != filter.kind) ||
                    !session_read::inRange(evidence.time,filter.fromTime,filter.toTime)) continue;
                if (filter.matchText && !filter.matchText(evidence)) {
                    result.truncated |= evidence.textTruncated;
                    continue;
                }
                attachMetadata(evidence,item);
                result.items.push_back(std::move(evidence));
            }
            if (!moreHistory(page,after,start)) return result;
            if (result.items.size() == filter.limit || Clock::now() >= deadline) break;
        }
        result.nextPosition = after;
        return result;
    });
}

std::optional<SessionEvent> sessionEvent(const std::filesystem::path& directory, const SessionTarget& target,
                                       std::string_view itemId) {
    return withSession(directory,target,[&](Client& client) -> std::optional<SessionEvent> {
        const auto item = readLocalItem(client,target,itemId,detailChars);
        if (!item) return {};
        auto result = event(*item);
        attachMetadata(result,*item);
        return result;
    });
}

std::optional<SessionCounts> sessionCounts(const std::filesystem::path& directory, const SessionTarget& target) {
    return withSession(directory,target,[&](Client& client) -> std::optional<SessionCounts> {
        Id messages = 0;
        Id after = -1;
        const auto deadline = Clock::now()+std::chrono::seconds(15);
        for (int scan = 0; scan < countScanPages && Clock::now() < deadline; ++scan) {
            const auto start = after;
            const auto page = readThread(client,target.threadId,"messages",100,1,{{"afterPosition",after}});
            for (const auto& item : array(page,"items")) {
                advancePosition(item,after);
                const auto kind = str(item,"type");
                if (localItem(item,target.threadId) && (kind == "user_message" || kind == "assistant_message")) ++messages;
            }
            if (!moreHistory(page,after,start))
                return SessionCounts{messages,position(member(member(page,"thread"),"runCount"))};
        }
        throw Failure(Failure::Kind::Wire,"T3 Session count did not finish");
    });
}

std::optional<SessionAttachment> sessionAttachment(const std::filesystem::path& directory, const SessionTarget& target,
                                                   std::string_view itemId, std::string_view attachmentId) {
    return withSession(directory,target,[&](Client& client) -> std::optional<SessionAttachment> {
        const auto item = readLocalItem(client,target,itemId,1);
        if (!item) return {};
        const auto files = item->find("attachments");
        if (files == item->end()) return {};
        if (!files->is_array()) invalidResponse();
        for (const auto& file : *files) {
            if (str(file,"id") != attachmentId) continue;
            auto value = attachment(file);
            if (value && file.contains("content")) value->content = str(file,"content");
            return value;
        }
        return {};
    });
}
}
