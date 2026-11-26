#include "bot_setup_storage.hpp"
#include "database.hpp"
#include "secret_storage.hpp"
#include "store_access.hpp"
#include "flock/decision_errors.hpp"
#include "flock/secrets.hpp"
#include "flock/policy.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace flockv0 {
void BotSetupStorage::validate(std::string_view bot, const Json& patch) const {
    if (!save) throw Conflict("Bot setup changes are unavailable");
    if (!patch.is_object() || patch.empty()) throw Conflict("Provide the Bot settings to update");
    for (auto entry = patch.begin(); entry != patch.end(); ++entry)
        if (std::ranges::find(botSetupFields, entry.key()) == std::end(botSetupFields))
            throw Conflict("This setting cannot be changed by the Bot");
    if (const auto avatar = patch.find("avatar"); avatar != patch.end()) {
        if (!avatar->is_object()) throw Conflict("Choose an avatar style");
        for (auto entry = avatar->begin(); entry != avatar->end(); ++entry)
            if (entry.key() != "kind" && entry.key() != "color" && entry.key() != "icon" && entry.key() != "example")
                throw Conflict("This avatar setting cannot be changed by the Bot");
    }
    if (secrets.redactor(bot).containsProtected(patch)) throw Conflict("Bot settings contain protected material");
}

Json BotSetupStorage::modelCatalog(const Json& patch) const {
    return loadCatalog && !field(patch, "reasoningEffort").empty() ? loadCatalog() : Json::array();
}

void BotSetupStorage::refreshPlannerModel(std::string_view bot, Json& config) const {
    if (save) StoreAccess::get(store).automationStorage.refreshPlannerModel(bot, config);
}

std::optional<Intake> BotSetupStorage::liveIntake(const IntakeResolver& resolve, std::string_view bot, std::string_view text) const {
    if (!save || !resolve) return {};
    try {
        return resolve(bot, text);
    } catch (const Conflict&) {
        return {};
    }
}

std::optional<Json> BotSetupStorage::recorded(std::string_view operation) {
    auto done = database.query("SELECT id FROM journal WHERE kind='bot_setup_updated' AND operation_id=?", {operation});
    if (!done.next()) return {};
    return Json{{"ok", true}, {"status", "applied"}, {"eventId", done.number(0)}};
}

Json BotSetupStorage::apply(std::string_view bot, std::string_view operation, const Json& patch, const Json& catalog, Seconds now, Id task,
                            Id message) {
    Id event = 0;
    save(store, bot,
         {patch, operation,
          [&](const Json& before, const Json& after, const Json& dependencies) {
              const auto scrub = secrets.redactor(bot);
              if (scrub.containsProtected(before) || scrub.containsProtected(after) || scrub.containsProtected(dependencies))
                  throw Conflict("Bot settings contain protected material");
              database.exec("INSERT INTO journal(time,kind,data,conversation_bot_id,operation_id,task_id)"
                            " VALUES(?,'bot_setup_updated',?,?,?,?)",
                            {now,
                             Json{{"before", before},
                                  {"after", after},
                                  {"dependencies", dependencies},
                                  {"undone", false},
                                  {"sourceMessageId", message}},
                             bot, operation, task ? Json(task) : Json(nullptr)});
              event = database.last();
          }},
         catalog);
    return {{"ok", true}, {"status", event ? "applied" : "unchanged"}, {"eventId", event}};
}

std::optional<PolicyOrigin> BotSetupStorage::taskOrigin(const TaskData& task) {
    if (!save || !task.sourceMessageId) return {};
    auto& storage = StoreAccess::get(store);
    auto origin = storage.policyApprovals.taskPolicyOrigin(task);
    if (!origin.anchor) return {};
    const auto message = storage.conversationTurns.loadTurnMessage(origin.anchor);
    if (message.bot == task.botId &&
        (message.principalSource == conversation::PrincipalSource::trusted ||
         message.principalSource == conversation::PrincipalSource::web) &&
        storage.permissions.allows(storage.conversationTurns.turnActor(message), Action::bot_settings, task.botId))
        return origin;
    return {};
}

bool BotSetupStorage::taskAvailable(const TaskData& task) {
    std::lock_guard lock(database.mutex);
    return taskOrigin(task).has_value();
}

bool BotSetupStorage::applyForTask(const TaskData& task, const Json& patch, Seconds now, Id attempt) {
    const auto catalog = modelCatalog(patch);
    auto& storage = StoreAccess::get(store);
    std::lock_guard lock(database.mutex);
    const auto current = storage.taskRecords.load(task.id);
    if (current.state != TaskState::deciding || current.generation != task.generation) return false;
    const auto key =
        std::string(taskSetupOperation) + std::to_string(task.id) + "/" + std::to_string(task.eventId) + "/" + std::to_string(attempt);
    if (recorded(key) || database.query("SELECT 1 FROM policy_approvals WHERE operation=? AND status='used'", {key}).next()) return true;
    const auto origin = taskOrigin(current);
    if (!origin) throw Conflict("You cannot change this Bot’s settings");
    validate(task.botId, patch);
    {
        Transaction transaction(database.db);
        const auto gate = storage.policyApprovals.approveAction(*origin, {Action::bot_settings, task.botId}, key, patch, now, false);
        transaction.commit();
        if (gate == ApprovalGate::waiting) return false;
        if (gate != ApprovalGate::allow) throw Conflict(std::string(approvalError(gate)));
    }
    apply(task.botId, key, patch, catalog, now, task.id);
    storage.policyApprovals.consumePolicyApproval(key);
    return true;
}

void BotSetupStorage::undo(const Person& actor, std::string_view bot, Id event) {
    std::lock_guard lock(database.mutex);
    if (!Policy{store}.allows(actor, Action::bot_settings, bot)) throw DecisionDenied{};
    if (!save) throw Conflict("Bot setup changes are unavailable");
    auto row = database.query("SELECT data,operation_id FROM journal WHERE id=? AND conversation_bot_id=? AND kind='bot_setup_updated'",
                              {event, bot});
    if (!row.next()) throw Conflict("Bot setup update not found");
    const auto data = row.json(0);
    if (data.at("undone") == true) return;
    const auto operation = row.string(1);
    auto expected = data.at("after");
    expected.update(data.at("dependencies"));
    save(store, bot,
         {data.at("before"), operation,
          [&](const Json&, const Json&, const Json&) {
              database.exec("UPDATE journal SET data=json_set(data,'$.undone',json('true')) WHERE id=?", {event});
              database.exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'bot_setup_undone',?,?)",
                            {unixTime(), Json{{"eventId", event}}, bot});
          },
          &expected},
         Json::array());
}

void undoBotSetup(Store& store, const Person& actor, std::string_view bot, Id event) {
    StoreAccess::get(store).botSetup.undo(actor, bot, event);
}
}
