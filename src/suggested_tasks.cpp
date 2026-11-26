#include "flock/suggested_tasks.hpp"
#include "conversation_storage.hpp"
#include "database.hpp"
#include "flock/limits.hpp"
#include "flock/decision_errors.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/secrets.hpp"
#include "store_access.hpp"
#include "storage.hpp"
#include "suggested_task_routing.hpp"
#include "suggested_tasks_storage.hpp"
#include "task_routing.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace flockv0 {
namespace {
std::string destinationLabel(const StartDestinations& destinations, std::string_view name) {
    if (destinations.targets.empty()) {
        const auto& route = destinations.primary;
        if (!name.empty() && name != route.environmentId) return std::string(name);
        if (route.projectId.empty()) return route.environmentId;
        return route.projectId + " / " + route.environmentId;
    }
    std::string label;
    for (const auto& target : destinations.targets) {
        if (!label.empty()) label += ", ";
        label += target.name;
    }
    return label;
}
}

std::size_t suggestTasks(SuggestionStorage& storage, TaskRouting& routing, const conversation::Message& message, const Json& items,
                         const LinkedTaskDestinations& linked, const TaskData* completedTask) {
    using namespace conversation_storage;
    if (!items.is_array() || items.empty() || items.size() > maxSuggestedTasks) {
        if (completedTask) return 0;
        throw ToolError("Suggest one to five follow-up tasks");
    }
    std::vector<SuggestedTaskInput> suggestions;
    for (const auto& item : items) {
        try {
            if (!item.is_object()) throw ToolError("Each follow-up must be an object");
            Json target = Json::object();
            for (const auto* key : {"environment", "environments"})
                if (item.contains(key)) target[key] = item.at(key);
            auto destinations = completedTask && target.empty()
                                    ? StartDestinations{.primary = {completedTask->environmentId, completedTask->projectId,
                                                                    completedTask->gatewayEnvironmentId.value_or("")}}
                                    : routing.startDestinations(message, target, false, nullptr, linked);
            auto& primary = destinations.primary;
            if (primary.projectId.empty()) primary.projectId = routing.rememberedT3Project(message.bot, primary.environmentId);
            auto label = destinationLabel(destinations, text(item, "environment", 256, false));
            suggestions.push_back({text(item, "request", limits::messages::maxBodyBytes, true),
                                   text(item, "doneWhen", limits::session_prompt::maxDecisionTextBytes, false),
                                   text(item, "reason", limits::session_prompt::maxDecisionTextBytes, true), std::move(label),
                                   std::move(destinations)});
        } catch (const ToolError&) {
            if (!completedTask) throw;
        } catch (const Conflict&) {
            if (!completedTask) throw;
        }
    }
    return storage.suggest(message.bot, message.id, completedTask ? std::optional{completedTask->id} : std::nullopt, suggestions);
}

Json suggestionRouting(const TaskData& task) {
    return {{"environmentId", task.environmentId}, {"projectId", task.projectId}, {"config", task.config}};
}

Json suggestedTaskRows(std::span<const SuggestedTask> tasks) {
    auto rows = Json::array();
    for (const auto& task : tasks) {
        const auto& item = task.input;
        Json row{{"id", task.id}, {"request", item.request}, {"reason", item.reason}, {"destination", item.destinationLabel}};
        switch (task.status) {
        case SuggestedTaskStatus::suggested: row["status"] = "suggested"; break;
        case SuggestedTaskStatus::started: row["status"] = "started"; break;
        case SuggestedTaskStatus::dismissed: row["status"] = "dismissed"; break;
        }
        if (task.taskId) {
            row["taskId"] = *task.taskId;
            row["taskEventId"] = task.taskEventId.value();
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

Json SuggestedTasks::start(const Person& actor, std::string_view bot, Id source, std::span<const SuggestedTaskStart> items,
                           const IntakeResolver& intake) {
    auto& store = StoreAccess::get(store_);
    std::lock_guard lock(store.database.mutex);
    Transaction transaction(store.database.db);
    if (!store.permissions.allows(actor, Action::bot_decide, bot)) throw DecisionDenied{};
    store.secretStorage.sharedRedactor().requirePublic(actor.id);
    if (items.empty() || items.size() > maxSuggestedTasks) throw Conflict("Choose one to five follow-ups");
    const auto card = store.suggestions.forCard(bot, source);
    auto result = Json::array();
    std::set<Id> selected;
    for (const auto& item : items) {
        if (!selected.insert(item.id).second) continue;
        const auto found = std::ranges::find(card, item.id, &SuggestedTask::id);
        if (found == card.end()) throw Conflict("That suggestion is no longer available");
        if (found->taskId) {
            result.push_back({{"id", item.id}, {"taskId", *found->taskId}});
            continue;
        }
        if (found->status != SuggestedTaskStatus::suggested) continue;
        const auto& saved = found->input;
        const auto& request = item.request.empty() ? saved.request : item.request;
        const auto& routes = saved.destinations;
        const auto requireLaunch = [&](const TaskRoute& route) {
            if (store.permissions.forbidden(bot, Action::environment_launch, route.environmentId)) throw DecisionDenied{};
        };
        requireLaunch(routes.primary);
        for (const auto& target : routes.targets) requireLaunch(target.route);
        const auto chosen = intake(bot, request);
        const auto id = store.taskOperations.startConversationTask({{"config", chosen.config}}, bot, request, routes, source,
                                                                   saved.doneWhen, {}, false, nullptr, &actor);
        store.suggestions.started(bot, source, item.id, id, request);
        result.push_back({{"id", item.id}, {"taskId", id}});
    }
    transaction.commit();
    return {{"started", std::move(result)}};
}

void SuggestedTasks::dismiss(const Person& actor, std::string_view bot, Id id) {
    auto& store = StoreAccess::get(store_);
    std::lock_guard lock(store.database.mutex);
    Transaction transaction(store.database.db);
    if (!store.permissions.allows(actor, Action::bot_decide, bot)) throw DecisionDenied{};
    if (!store.suggestions.dismiss(bot, id)) throw Conflict("That suggestion is no longer available");
    transaction.commit();
}
}
