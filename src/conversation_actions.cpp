#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <set>

namespace flockv0 {
using conversation_storage::verifiedEnvironment;

Id Store::Impl::startConversationTask(const Json& routing, std::string_view bot, std::string_view request,
                                      std::string_view environment, Id messageId, const Json& source,
                                      bool resume) {
    const auto destination = environment.empty() ? field(routing,"environmentId")
        : verifiedEnvironment(routing,routing,environment);
    const auto id = submit({std::string(bot),std::string(request),destination,field(routing,"projectId"),
                            routing.at("config"),{},0},messageId,source);
    auto task = load(id);
    // Establish provenance at intake, while both the original message and the routed request are available.
    auto message = query("SELECT text FROM messages WHERE id=? AND bot_id=?", {messageId,bot});
    task["sourceMessageCoversFullRequest"] = source.is_null() && message.next() && message.string(0) == request;
    if (resume) {
        const auto priorId = source.at("taskId").get<Id>();
        const auto prior = load(priorId);
        const auto criteria = prior.value("requestedDoneWhen",field(prior,"doneWhen"));
        if (!criteria.empty()) task["requestedDoneWhen"] = criteria;
        const auto pending = prior.value("cancelledInstructions",Json::array());
        std::set<std::pair<Id,int>> pendingIds;
        for (const auto& item : pending)
            pendingIds.emplace(item.at("messageId").get<Id>(),item.at("step").get<int>());

        // Excerpt only earlier deliveries. Pending directions follow them in acceptance order.
        auto rows = query("SELECT data FROM journal WHERE task_id=? AND kind='instruction_accepted'"
                          " ORDER BY id DESC",{priorId});
        Json delivered = Json::array();
        std::size_t remaining = 8000;
        while (remaining && delivered.size() < 20 && rows.next()) {
            const auto item = rows.json(0);
            if (pendingIds.contains({item.at("messageId").get<Id>(),item.at("step").get<int>()})) continue;
            const auto text = bounded(field(item,"text"),remaining);
            remaining -= text.size();
            delivered.push_back({{"messageId",item.at("messageId")},{"step",item.at("step")},{"text",text}});
        }
        std::reverse(delivered.begin(),delivered.end());
        task["intent"] = taskIntent(prior);
        task["restartContext"] = {{"taskId",priorId},{"deliveredInstructions",std::move(delivered)}};
        for (const auto& item : pending) {
            Json entry = {{"messageId",item.at("messageId")},{"step",item.at("step")},{"text",item.at("text")},
                          {"revisionId",nullptr},{"conflicting",false}};
            if (item.contains("source")) {
                const auto& source = item.at("source");
                entry["source"] = source;
                auto& refs = task["sourceResultRefs"];
                if (std::ranges::none_of(refs,[&](const Json& ref) { return ref.at("messageId") == source.at("messageId"); }))
                    refs.push_back(source);
            }
            task["pendingInstructions"].push_back(entry);
            journal(id,"instruction_accepted",entry);
        }
    }
    save(task);
    return id;
}

bool Store::Impl::acceptInstruction(Json& task, Id messageId, int step, std::string_view instruction,
                                    const Json& source, const Json& routing, const TaskChange* change) {
    Id revision = task.value("currentRevision",Id{});
    bool conflicting = task.value("replanning",false);
    if (change) {
        const auto oldIntent = field(task,"intent");
        const auto oldDone = task.value("requestedDoneWhen",field(task,"doneWhen"));
        const auto intent = change->intent.empty() ? oldIntent : change->intent;
        const auto done = change->doneWhen.empty() ? oldDone : change->doneWhen;
        const auto environment = verifiedEnvironment(task,routing,change->environment);
        const bool criteriaChanged = oldDone != done;
        const bool intentChanged = oldIntent != intent;
        if (criteriaChanged || intentChanged) {
            if (task.contains("currentRevision")) revision = addRevision(task,intent,done,messageId);
            else {
                task["intent"] = intent;
                if (criteriaChanged) {
                    task["requestedDoneWhen"] = done;
                    task["criteriaSourceMessageId"] = messageId;
                }
            }
            if (intentChanged) task["sourceMessageCoversFullRequest"] = false;
            conflicting = true;
        }
        if (!environment.empty() && environment != task.value("pendingEnvironmentId",field(task,"environmentId"))) {
            task["pendingEnvironmentId"] = environment;
            conflicting = true;
        }
    }
    Json pending = {{"messageId",messageId},{"step",step},{"text",instruction},{"revisionId",revision},
                    {"conflicting",conflicting}};
    if (!source.is_null()) {
        pending["source"] = source;
        auto& references = task["sourceResultRefs"];
        if (references.is_null()) references = Json::array();
        if (std::ranges::none_of(references,[&](const Json& ref) { return ref.at("messageId") == source.at("messageId"); }))
            references.push_back(source);
    }
    if (conflicting) task["replanning"] = true;
    if (!task.contains("pendingInstructions")) task["pendingInstructions"] = Json::array();
    task["pendingInstructions"].push_back(pending);
    journal(task.at("id"),"instruction_accepted",pending);
    save(task);
    return conflicting;
}
}
