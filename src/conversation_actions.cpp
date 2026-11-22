#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>

namespace flockv0 {
using conversation_storage::verifiedEnvironment;

Id Store::Impl::startConversationTask(const Json& routing, std::string_view bot, std::string_view request,
                                      std::string_view environment, Id messageId, const Json& source,
                                      bool resume) {
    const auto destination = environment.empty() ? field(routing,"environmentId")
        : verifiedEnvironment(routing,routing,environment);
    const auto id = submit({std::string(bot),std::string(request),destination,field(routing,"projectId"),
                            routing.at("config"),{},0},messageId,source);
    if (!resume) return id;
    const auto priorId = source.at("taskId").get<Id>();
    const auto prior = load(priorId);
    auto task = load(id);
    if (const auto criteria = prior.value("requestedDoneWhen",field(prior,"doneWhen")); !criteria.empty())
        task["requestedDoneWhen"] = criteria;
    task["restartContext"] = {{"intent",prior.value("intent",prior.at("request"))},{"taskId",priorId},
                              {"pendingInstructions",prior.value("cancelledInstructions",Json::array())}};
    auto rows = query(R"SQL(
SELECT entry.value FROM outbox, json_each(outbox.data,'$.instructions') entry
WHERE outbox.task_id=? AND outbox.phase='accepted'
ORDER BY outbox.rowid DESC, entry.key DESC LIMIT 20
)SQL",{priorId});
    auto earlier = Json::array();
    std::size_t remaining = 8000;
    while (remaining && rows.next()) {
        auto instruction = rows.json(0);
        instruction["text"] = bounded(field(instruction,"text"),remaining);
        remaining -= field(instruction,"text").size();
        earlier.push_back(std::move(instruction));
    }
    std::reverse(earlier.begin(),earlier.end());
    auto& instructions = task["restartContext"]["pendingInstructions"];
    instructions.insert(instructions.begin(),earlier.begin(),earlier.end());
    save(task);
    return id;
}

bool Store::Impl::acceptInstruction(Id taskId, Id revision, Id messageId, int step, std::string_view instruction,
                                    const Json& source, const Json& routing, const TaskChange* change) {
    auto task = load(taskId);
    bool conflicting = change != nullptr;
    std::string environment;
    if (change) {
        const auto oldIntent = field(task,"intent").empty() ? field(task,"request") : field(task,"intent");
        const auto oldDone = field(task,"doneWhen");
        const auto intent = change->intent.empty() ? oldIntent : change->intent;
        const auto done = change->doneWhen.empty() ? oldDone : change->doneWhen;
        if (!oldDone.empty() && (intent != oldIntent || done != oldDone))
            revision = addRevision(task,intent,done,messageId);
        else if (oldDone.empty()) {
            task["intent"] = intent;
            if (!change->doneWhen.empty()) {
                task["requestedDoneWhen"] = change->doneWhen;
                task["criteriaSourceMessageId"] = messageId;
            }
        }
        environment = verifiedEnvironment(task,routing,change->environment);
        conflicting = !change->doneWhen.empty() || !change->intent.empty() || environment.empty() ||
                      environment != field(task,"environmentId") || task.value("replanning",false);
    }
    Json pending = {{"messageId",messageId},{"step",step},{"text",instruction},{"revisionId",revision},
                    {"conflicting",conflicting}};
    if (!source.is_null()) {
        pending["source"] = source;
        auto& references = task["sourceResultRefs"];
        if (references.is_null()) references = Json::array();
        if (std::ranges::find(references,source) == references.end()) references.push_back(source);
    }
    if (!environment.empty()) {
        pending["environmentId"] = environment;
        if (conflicting) task["pendingEnvironmentId"] = environment;
    }
    if (conflicting) {
        pending["replaceKey"] = !environment.empty() ? "environment" : !change->doneWhen.empty() ? "criteria"
                                : !change->intent.empty() ? "intent" : "target";
        task["replanning"] = true;
    }
    if (!task.contains("pendingInstructions")) task["pendingInstructions"] = Json::array();
    task["pendingInstructions"].push_back(pending);
    journal(taskId,"instruction_accepted",pending);
    save(task);
    return conflicting;
}
}
