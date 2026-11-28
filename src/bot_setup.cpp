#include "bot_setup_storage.hpp"
#include "database.hpp"
#include "secret_storage.hpp"
#include "store_access.hpp"
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
    if (const auto model = patch.find("model");
        model != patch.end() && (!model->is_string() || model->get_ref<const std::string&>().empty()))
        throw Conflict("Choose a model for the Bot");
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

void BotSetupStorage::undo(const Person& actor, std::string_view bot, Id event) {
    std::lock_guard lock(database.mutex);
    if (!Policy{store}.allows(actor, Action::bot_settings, bot)) throw AccessDenied{};
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
