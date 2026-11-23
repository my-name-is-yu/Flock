#include "storage.hpp"
#include "conversation_storage.hpp"
#include "flock/session_read.hpp"

#include <map>
#include <set>

namespace flockv0 {
namespace {
constexpr auto unavailable = "Session evidence not found";
constexpr auto searchTimedOut = "Session search timed out. Retry with the same filters and cursor.";
constexpr std::size_t searchCandidateLimit = 64;
constexpr auto searchTimeLimit = std::chrono::seconds(2);
using Clock = std::chrono::steady_clock;

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

struct Filters {
    explicit Filters(const Json& args)
        : query(argument(args,"query")), from(argument(args,"fromTime",30)), to(argument(args,"toTime",30)) {
        if ((!from.empty() && !session_read::utcTimestamp(from)) ||
            (!to.empty() && !session_read::utcTimestamp(to)) ||
            (!from.empty() && !session_read::inRange(from,{},to)))
            throw Conflict("Use an ordered UTC date-time range");
    }
    std::string query, from, to;
};

enum class RefKind { session, search, events, event, attachment, searchPosition };

struct Reference {
    SessionTarget target;
    Json payload;
};

std::string_view reference(const Json& args, const char* key) {
    const auto value = args.find(key);
    if (value == args.end() || !value->is_string()) return {};
    const auto& ref = value->get_ref<const std::string&>();
    if (ref.size() != 38 || !ref.starts_with("flock-") ||
        ref.find_first_not_of("0123456789abcdef",6) != ref.npos) return {};
    return ref;
}

bool hasCursor(const Json& args) {
    const auto cursor = args.find("cursor");
    return cursor != args.end() && !cursor->is_null();
}

Json scopedResult(Json result, Json sessions = Json::array()) {
    return {{"result",std::move(result)},{"sessions",std::move(sessions)}};
}
}

class Store::Impl::SessionEvidence {
public:
    SessionEvidence(Store::Impl& store, Policy& policy, std::string_view bot, std::string_view channelScope, T3& t3)
        : store_(store), policy_(policy), bot_(bot), channelScope_(channelScope), t3_(t3) {}

    Json search(const Json& args, const Json& routing) {
        const Filters filters(args);
        const auto project = argument(args,"project");
        const auto status = argument(args,"status",128);
        const auto limit = pageSize(args);
        const auto key = Json::array({filters.query,project,status,filters.from,filters.to});
        const auto configuredProject = field(routing,"projectId");
        std::set<std::string> environments;
        if (channelScope_.empty()) {
            const auto environment = field(routing,"environmentId");
            if (!environment.empty()) environments.insert(environment);
            for (const auto& id : routing.at("config").value("conversationEnvironments",Json::object()))
                environments.insert(id.get<std::string>());
        }
        const auto scopedKey = Json::array({key,channelScope_,configuredProject,environments});
        std::optional<Reference> anchor;
        Json progress{{"recorded",Json::array({"",""})},{"catalog",Json::object()}};
        if (!configuredProject.empty())
            for (const auto& environment : environments) progress["catalog"][environment] = 0;
        std::vector<SessionTarget> batch;
        const auto cursor = reference(args,"cursor");
        if (hasCursor(args)) {
            anchor = resolve(cursor,RefKind::search);
            const auto position = anchor ? anchor : resolve(cursor,RefKind::searchPosition);
            if (!position || !position->payload.is_object() || position->payload.at("scope") != scopedKey ||
                (anchor && (!permitted(anchor->target) || !readable(anchor->target,Clock::now()+searchTimeLimit))))
                return scopedResult(failure(unavailable));
            progress = position->payload.at("progress");
            for (const auto& target : position->payload.at("remaining"))
                batch.push_back({target.at(0).get<std::string>(),target.at(1).get<std::string>()});
        }
        if (batch.empty()) {
            batch = candidates(configuredProject,environments,progress);
            if (!cursor.empty() && !batch.empty()) omitEmitted(batch,cursor);
        }
        const bool catalogTruncated = !progress.at("catalog").empty();
        const auto scrub = store_.sharedRedactor();
        std::vector<Candidate> matches;
        std::vector<SessionTarget> incomplete;
        const auto scanLimit = std::min(searchCandidateLimit,batch.size());
        const auto scanDeadline = Clock::now()+searchTimeLimit;
        std::size_t scanned = 0;
        bool timedOut = false;
        bool skippedSlowCandidate = false;
        for (;;) {
            while (matches.size() <= limit && scanned < scanLimit) {
                if (Clock::now() >= scanDeadline) {
                    timedOut = true;
                    break;
                }
                const auto& target = batch[scanned];
                std::optional<SessionMetadata> item;
                try {
                    if (permitted(target)) item = t3_.sessionMetadata(target,!filters.query.empty(),scanDeadline);
                } catch (const SessionReadTimeout&) {
                    timedOut = true;
                    if (scanned == 0) {
                        skippedSlowCandidate = true;
                        ++scanned;
                    }
                    break;
                }
                ++scanned;
                if (!item || (channelScope_.empty() && item->projectId != configuredProject) ||
                    (!project.empty() && item->projectId != project &&
                     !session_read::contains(scrub.text(item->project),project)) ||
                    (!status.empty() && item->status != status) ||
                    !session_read::inRange(item->updatedAt,filters.from,filters.to)) continue;
                auto match = matchText(scrub,*item,filters.query);
                if (match.matched)
                    matches.push_back({target,std::move(*item),std::move(match)});
                else if (match.incomplete) incomplete.push_back(target);
            }

            if (timedOut && scanned == 0) return scopedResult(failure(searchTimedOut));
            std::lock_guard lock(store_.mutex);
            Transaction transaction(store_.db);
            if (anchor && !permitted(anchor->target)) return scopedResult(failure(unavailable));
            std::erase_if(matches,[&](const Candidate& row) { return !permitted(row.target); });
            if (matches.size() <= limit && !timedOut && scanned < scanLimit) continue;

            Json result{{"ok",true},{"sessions",Json::array()},{"nextCursor",nullptr},{"truncated",false}};
            auto& items = result["sessions"];
            auto scopes = Json::array();
            if (anchor) scopes.push_back(issue(RefKind::session,anchor->target,nullptr));
            const auto latestScrub = store_.sharedRedactor();
            const auto unsearched = std::ranges::find_if(incomplete,[&](const SessionTarget& target) { return permitted(target); });
            bool truncated = catalogTruncated || skippedSlowCandidate || unsearched != incomplete.end();
            if (unsearched != incomplete.end()) scopes.push_back(issue(RefKind::session,*unsearched,nullptr));
            auto nextIndex = scanned;
            auto emitted = Json::array();
            for (const auto& [target,item,match] : matches) {
                if (items.size() == limit) {
                    nextIndex = static_cast<std::size_t>(std::ranges::find(batch,target)-batch.begin());
                    scopes.push_back(issue(RefKind::session,target,nullptr));
                    break;
                }
                truncated |= match.incomplete;
                const auto display = [&](std::string_view text, std::size_t bytes) {
                    return boundedField(latestScrub,text,bytes,truncated);
                };
                const auto ref = issue(RefKind::session,target,nullptr);
                scopes.push_back(ref);
                emitted.push_back(Json::array({target.environmentId,target.threadId}));
                items.push_back({{"sessionRef",ref},
                    {"title",display(item.title,240)},{"project",display(item.project,240)},
                    {"status",display(item.status,128)},{"createdAt",display(item.createdAt,30)},
                    {"updatedAt",display(item.updatedAt,30)},{"matchExcerpt",display(match.excerpt,480)}});
            }
            if (nextIndex < batch.size() || catalogTruncated || !progress.at("recorded").is_null()) {
                auto remaining = Json::array();
                for (std::size_t i = nextIndex; i < batch.size(); ++i)
                    remaining.push_back(Json::array({batch[i].environmentId,batch[i].threadId}));
                const bool matchAnchor = nextIndex < scanned;
                const auto target = matchAnchor ? matches[limit-1].target : SessionTarget{};
                result["nextCursor"] = issue(matchAnchor ? RefKind::search : RefKind::searchPosition,target,
                    {{"scope",scopedKey},{"progress",progress},{"remaining",std::move(remaining)},
                     {"previous",cursor},{"emitted",std::move(emitted)}});
            }
            result["truncated"] = truncated;
            transaction.commit();
            return scopedResult(std::move(result),std::move(scopes));
        }
    }

    ApprovalGate authorizeEventTool(const conversation::Message& message, int step, const Decision& call) {
        std::lock_guard lock(store_.mutex);
        Transaction tx(store_.db);
        const auto session = resolve(reference(call.args,"sessionRef"),RefKind::session);
        if (!session || !inChannelScope(session->target)) return ApprovalGate::allow;
        const auto resource = Resource::sessionRead(session->target.environmentId,session->target.threadId);
        if (policy_.forbidden(bot_,resource.action,resource.target)) return ApprovalGate::allow;
        const auto key = "turn/" + std::to_string(message.id) + "/" + std::to_string(step);
        const auto gate = store_.approveAction(store_.policyOrigin(message),resource,key,
                                               Json::array({call.tool,call.args}),store_.policyClock());
        if (gate == ApprovalGate::allow) approved_ = session->target;
        tx.commit();
        return gate;
    }

    Json events(const Json& args) {
        const auto operation = argument(args,"operation",32);
        if (operation == "list" || operation == "search") return eventPage(args,operation);
        if (operation != "counts" && operation != "read_event" && operation != "get_attachment")
            throw Conflict("Invalid Session event operation");
        const auto session = authorizedTarget(args);
        if (!session) return scopedResult(failure(unavailable));
        const auto& target = *session;
        if (operation == "counts") {
            return finish(target,[&] { return t3_.sessionCounts(target); },
                [](const SessionCounts& counts, const SecretRedactor&, Json& result) {
                    result["counts"] = {{"messages",counts.messages},{"runs",counts.runs}};
                });
        }
        if (operation == "read_event") {
            const auto id = scoped(args,"eventRef",RefKind::event,target);
            if (!id) return scopedResult(failure(unavailable));
            return finish(target,[&] { return t3_.sessionEvent(target,id->get<std::string>()); },
                [&](const SessionEvent& item, const SecretRedactor& scrub, Json& result) {
                    result["event"] = eventView(item,target,scrub,true);
                    result["truncated"] = result["event"].at("truncated");
                });
        }
        const auto id = scoped(args,"attachmentRef",RefKind::attachment,target);
        if (!id) return scopedResult(failure(unavailable));
        return finish(target,[&] {
            return t3_.sessionAttachment(target,id->at(0).get<std::string>(),id->at(1).get<std::string>());
        },[&](const SessionAttachment& file, const SecretRedactor& scrub, Json& result) {
            bool truncated = file.truncated;
            auto view = attachmentView(file,reference(args,"attachmentRef"),scrub,truncated);
            if (file.content) view["content"] = boundedField(scrub,*file.content,8192,truncated,file.truncated);
            result["attachment"] = std::move(view);
            result["truncated"] = truncated;
        });
    }

    void authorizeHistory(Json& steps) const {
        std::map<std::pair<std::string,bool>,std::optional<SessionTarget>> checked;
        struct SavedEvidence { Json* result; bool accessible, dispatched; };
        std::vector<SavedEvidence> evidence;
        for (auto& step : steps) {
            const auto& tool = step.at("tool");
            if (tool != "flock_session_search" && tool != "flock_session_events") continue;
            const auto& outcome = step.at("result");
            if (!outcome.contains("sessions") && outcome.value("ok",true) == false) continue;
            const auto& refs = outcome.at("sessions");
            const bool dispatched = tool == "flock_session_events";
            for (const auto& token : refs) {
                const auto& ref = token.get_ref<const std::string&>();
                auto [entry, fresh] = checked.try_emplace({ref,dispatched});
                if (!fresh) continue;
                const auto bound = resolve(ref,RefKind::session);
                if (bound && permitted(bound->target,dispatched) && readable(bound->target)) entry->second = bound->target;
            }
            const auto& args = step.at("args");
            const bool attachment = !refs.empty() && tool == "flock_session_events" &&
                field(args,"operation") == "get_attachment";
            const bool allowed = !attachment || attachmentReadable(checked.at({refs.at(0).get<std::string>(),dispatched}),args,dispatched);
            evidence.push_back({&step.at("result"),allowed,dispatched});
        }
        if (evidence.empty()) return;
        std::lock_guard lock(store_.mutex);
        Transaction transaction(store_.db,Transaction::Mode::read);
        const auto scrub = store_.sharedRedactor();
        for (const auto& [result,resourceAllowed,dispatched] : evidence) {
            auto saved = std::move(*result);
            const auto allowed = resourceAllowed && std::ranges::all_of(saved.at("sessions"),[&](const Json& token) {
                const auto& target = checked.at({token.get_ref<const std::string&>(),dispatched});
                return target && permitted(*target,dispatched);
            });
            *result = scrub.record(allowed ? std::move(saved.at("result")) : failure(unavailable),
                                   SecretRedactor::Fields::external);
        }
        transaction.commit();
    }

private:
    struct TextMatch {
        bool matched = false, incomplete = false;
        std::string excerpt;
    };
    struct Candidate {
        SessionTarget target;
        SessionMetadata metadata;
        TextMatch match;
    };
    Store::Impl& store_;
    Policy& policy_;
    std::string_view bot_;
    std::string_view channelScope_;
    T3& t3_;
    std::optional<SessionTarget> approved_;

    bool inChannelScope(const SessionTarget& target) const {
        if (!channelScope_.empty()) {
            std::lock_guard lock(store_.mutex);
            if (!store_.query(R"SQL(
SELECT 1 FROM runs r JOIN tasks t ON t.id=r.task_id JOIN channel_tasks c ON c.task_id=t.id
WHERE t.bot_id=? AND c.scope_key=? AND r.environment_id=? AND r.thread_id=?
)SQL",{bot_,channelScope_,target.environmentId,target.threadId}).next()) return false;
        }
        return true;
    }

    bool permitted(const SessionTarget& target, bool dispatched = false) const {
        if (!inChannelScope(target)) return false;
        const auto resource = Resource::sessionRead(target.environmentId,target.threadId);
        if (dispatched || (approved_ && target == *approved_)) return !policy_.forbidden(bot_,resource.action,resource.target);
        return policy_.allows(bot_,resource);
    }

    bool readable(const SessionTarget& target, SessionReadDeadline deadline = {}) const {
        try {
            return t3_.sessionMetadata(target,false,deadline).has_value();
        } catch (const SessionReadTimeout&) {
            throw;
        } catch (const std::exception&) {
            return false;
        }
    }

    bool attachmentReadable(const std::optional<SessionTarget>& target, const Json& args, bool dispatched) const {
        if (!target || !permitted(*target,dispatched)) return false;
        try {
            const auto id = scoped(args,"attachmentRef",RefKind::attachment,*target);
            return id && t3_.sessionAttachment(*target,id->at(0).get<std::string>(),id->at(1).get<std::string>()).has_value();
        } catch (const std::exception&) {
            return false;
        }
    }

    std::vector<SessionTarget> candidates(const std::string& project, const std::set<std::string>& environments,
                                          Json& progress) const {
        std::vector<SessionTarget> result;
        auto& recorded = progress.at("recorded");
        if (!recorded.is_null()) {
            std::lock_guard lock(store_.mutex);
            auto rows = store_.query(R"SQL(
SELECT DISTINCT r.environment_id,r.thread_id FROM runs r JOIN tasks t ON t.id=r.task_id
LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE ((?5='' AND ?6<>'' AND t.project_id=?6 AND r.environment_id IN (SELECT value FROM json_each(?7)))
       OR (?5<>'' AND t.bot_id=?1 AND c.scope_key=?5))
  AND (r.environment_id,r.thread_id) > (?2,?3)
ORDER BY r.environment_id,r.thread_id LIMIT ?4
)SQL",{bot_,recorded.at(0),recorded.at(1),searchCandidateLimit+1,channelScope_,project,Json(environments).dump()});
            while (rows.next()) result.push_back({rows.string(0),rows.string(1)});
            recorded = nullptr;
            if (result.size() > searchCandidateLimit) {
                result.pop_back();
                recorded = Json::array({result.back().environmentId,result.back().threadId});
            }
        }
        auto& catalog = progress.at("catalog");
        const auto deadline = Clock::now()+searchTimeLimit;
        for (auto it = catalog.begin(); it != catalog.end() && Clock::now() < deadline;) {
            const auto& id = it.key();
            if (!policy_.allows(bot_,Action::environment_inspect,id)) {
                it = catalog.erase(it);
                continue;
            }
            const auto offset = it.value().get<Id>();
            try {
                auto [threads,next] = t3_.projectSessions(id,project,offset,deadline);
                for (auto& thread : threads) result.push_back({id,std::move(thread)});
                if (next) {
                    it.value() = *next;
                    ++it;
                } else it = catalog.erase(it);
            } catch (const SessionReadTimeout&) {
                break;
            }
        }
        std::ranges::sort(result,[](const auto& a, const auto& b) {
            return std::tie(a.environmentId,a.threadId) < std::tie(b.environmentId,b.threadId);
        });
        result.erase(std::unique(result.begin(),result.end()),result.end());
        return result;
    }

    void omitEmitted(std::vector<SessionTarget>& batch, std::string_view cursor) const {
        auto targets = Json::array();
        for (const auto& target : batch)
            targets.push_back(Json::array({target.environmentId,target.threadId}));
        std::set<std::pair<std::string,std::string>> emitted;
        std::lock_guard lock(store_.mutex);
        // Each cursor stores only its own page so lineage survives restarts without copying prior pages.
        auto rows = store_.query(R"SQL(
WITH RECURSIVE pages(payload) AS (
    SELECT payload FROM session_read_refs WHERE ref=?1 AND bot=?2
    UNION ALL
    SELECT r.payload FROM pages JOIN session_read_refs r
      ON r.ref=json_extract(pages.payload,'$.previous') AND r.bot=?2
)
SELECT DISTINCT json_extract(target.value,'$[0]'),json_extract(target.value,'$[1]')
FROM pages, json_each(pages.payload,'$.emitted') entry
JOIN json_each(?3) target ON target.value=entry.value
)SQL",{cursor,bot_,targets.dump()});
        while (rows.next()) emitted.emplace(rows.string(0),rows.string(1));
        std::erase_if(batch,[&](const SessionTarget& target) {
            return emitted.contains({target.environmentId,target.threadId});
        });
    }

    std::optional<Reference> resolve(std::string_view ref, RefKind kind) const {
        if (ref.empty()) return {};
        std::lock_guard lock(store_.mutex);
        auto row = store_.query("SELECT environment,thread,payload FROM session_read_refs WHERE ref=? AND bot=? AND kind=?",
                                {ref,bot_,static_cast<int>(kind)});
        if (!row.next()) return {};
        return Reference{{row.string(0),row.string(1)},row.json(2)};
    }

    std::optional<Json> scoped(const Json& args, const char* key, RefKind kind, const SessionTarget& target) const {
        auto resolved = resolve(reference(args,key),kind);
        if (!resolved || target != resolved->target) return {};
        return std::move(resolved->payload);
    }

    std::optional<SessionTarget> authorizedTarget(const Json& args) const {
        auto session = resolve(reference(args,"sessionRef"),RefKind::session);
        if (!session || !permitted(session->target)) return {};
        return std::move(session->target);
    }

    std::string issue(RefKind kind, const SessionTarget& target, const Json& payload) const {
        const auto key = Json::array({bot_,static_cast<int>(kind),target.environmentId,target.threadId,payload.dump()});
        auto row = store_.query("SELECT ref FROM session_read_refs WHERE bot=? AND kind=? AND environment=? AND thread=? AND payload=?",key);
        if (row.next()) return row.string(0);
        auto values = key;
        const auto ref = actionId();
        values.push_back(ref);
        store_.exec("INSERT INTO session_read_refs(bot,kind,environment,thread,payload,ref) VALUES(?,?,?,?,?,?)",values);
        return ref;
    }

    static TextMatch matchText(const SecretRedactor& scrub, const SessionMetadata& item, std::string_view query) {
        if (query.empty() || session_read::contains(scrub.text(item.title),query) ||
            session_read::contains(scrub.text(item.project),query)) return {true,false,{}};
        bool incomplete = item.moreMessages;
        for (const auto& message : item.messages) {
            auto text = scrub.text(message.text,message.truncated);
            if (session_read::contains(text,query)) return {true,message.truncated,std::move(text)};
            incomplete |= message.truncated;
        }
        return {false,incomplete,{}};
    }

    static std::string boundedField(const SecretRedactor& scrub, std::string_view value, std::size_t bytes,
                                    bool& truncated, bool sourceTruncated = false) {
        const auto safe = scrub.text(value,sourceTruncated);
        auto text = bounded(safe,bytes);
        truncated |= text.size() < safe.size();
        return text;
    }

    Json attachmentView(const SessionAttachment& file, std::string_view ref, const SecretRedactor& scrub,
                        bool& truncated) const {
        return {{"attachmentRef",ref},{"name",boundedField(scrub,file.name,240,truncated)},
                {"mimeType",boundedField(scrub,file.mimeType,128,truncated)}};
    }

    Json eventView(const SessionEvent& event, const SessionTarget& target, const SecretRedactor& scrub,
                   bool detail) const {
        const auto maxText = detail ? 8192U : 480U;
        bool truncated = event.textTruncated || event.attachmentsTruncated;
        const auto display = [&](std::string_view text, std::size_t bytes, bool sourceTruncated = false) {
            return boundedField(scrub,text,bytes,truncated,sourceTruncated);
        };
        const auto text = display(event.text,maxText,event.textTruncated);
        Json result{{"eventRef",issue(RefKind::event,target,event.id)},{"timestamp",display(event.time,30)},
                    {"kind",display(event.kind,128)},{"status",display(event.status,128)},
                    {"title",display(event.title,240)},{detail ? "text" : "excerpt",text}};
        if (detail && event.kind == "error") result["error"] = text;
        auto& files = result["attachments"] = Json::array();
        for (const auto& file : event.attachments) {
            if (files.size() == sessionPageLimit) {
                truncated = true;
                break;
            }
            files.push_back(attachmentView(file,issue(RefKind::attachment,target,Json::array({event.id,file.id})),
                                           scrub,truncated));
        }
        result["truncated"] = truncated;
        return result;
    }

    template<class Read, class Format>
    Json finish(const SessionTarget& target, Read read, Format format) {
        decltype(read()) value;
        try {
            value = read();
        } catch (const std::exception&) {
            return scopedResult(failure(unavailable));
        }
        if (!value) return scopedResult(failure(unavailable));
        std::lock_guard lock(store_.mutex);
        Transaction transaction(store_.db);
        if (!permitted(target)) return scopedResult(failure(unavailable));
        Json result{{"ok",true},{"truncated",false},{"nextCursor",nullptr}};
        format(*value,store_.sharedRedactor(),result);
        auto scopes = Json::array({issue(RefKind::session,target,nullptr)});
        transaction.commit();
        return scopedResult(std::move(result),std::move(scopes));
    }

    Json eventPage(const Json& args, const std::string& operation) {
        const Filters filters(args);
        const auto kind = argument(args,"kind",128);
        const auto limit = pageSize(args);
        const auto session = authorizedTarget(args);
        if (!session) return scopedResult(failure(unavailable));
        const auto& target = *session;
        const auto key = Json::array({operation,filters.query,kind,filters.from,filters.to});
        std::optional<Id> after;
        if (hasCursor(args)) {
            const auto saved = scoped(args,"cursor",RefKind::events,target);
            if (!saved || saved->at(0) != key) return scopedResult(failure(unavailable));
            after = saved->at(1).get<Id>();
        }
        SessionEventFilter filter{kind,filters.from,filters.to,limit,after,{}};
        if (!filters.query.empty()) {
            const auto scrub = store_.sharedRedactor();
            filter.matchText = [scrub,query = filters.query](const SessionEvent& item) {
                return session_read::contains(scrub.text(item.text,item.textTruncated),query) ||
                       session_read::contains(scrub.text(item.title),query);
            };
        }
        return finish(target,[&] {
            return t3_.sessionEvents(target,filter);
        },[&](const SessionEventPage& page, const SecretRedactor& scrub, Json& result) {
            if (page.items.size() > limit) throw std::logic_error("Invalid Session event page");
            auto& items = result["events"] = Json::array();
            bool truncated = page.truncated;
            for (const auto& item : page.items) {
                auto view = eventView(item,target,scrub,false);
                truncated |= view.at("truncated").get<bool>();
                items.push_back(std::move(view));
            }
            if (page.nextPosition) result["nextCursor"] = issue(RefKind::events,target,Json::array({key,*page.nextPosition}));
            result["truncated"] = truncated;
        });
    }
};

Json Store::Impl::sessionReadTool(const conversation::Message& message, int step, const Decision& decision, T3& t3) {
    Policy policy(*this);
    SessionEvidence evidence(*this,policy,message.bot,message.channelScope,t3);
    try {
        if (!decision.args.is_object()) throw Conflict("Tool arguments must be an object");
        if (decision.tool == "flock_session_search") return evidence.search(decision.args,message.routing);
        const auto gate = evidence.authorizeEventTool(message,step,decision);
        if (gate == ApprovalGate::waiting) return {{"policyWaiting",true}};
        if (gate != ApprovalGate::allow) return scopedResult(failure(std::string(approvalError(gate))));
        return evidence.events(decision.args);
    } catch (const Conflict& error) {
        return scopedResult(failure(error.what()));
    } catch (const SessionReadTimeout&) {
        return scopedResult(failure(searchTimedOut));
    } catch (const StorageError&) {
        throw;
    } catch (const std::exception&) {
        return scopedResult(failure("Session evidence could not be read"));
    }
}

void Store::Impl::authorizeSessionHistory(std::string_view bot, std::string_view channelScope, Json& steps, T3& t3) {
    Policy policy(*this);
    SessionEvidence(*this,policy,bot,channelScope,t3).authorizeHistory(steps);
}
}
