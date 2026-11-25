#include "storage.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>

namespace flockv0 {
namespace {
constexpr std::size_t maxRequestBytes = 16'000;

void validateText(std::string_view value, std::string_view name, std::size_t maximum) {
    if (value.size() > maximum || value.find_first_not_of(" \r\n\t") == value.npos) throw Conflict("Invalid " + std::string(name));
}

std::string requiredText(const Json& args, std::string_view name, std::size_t maximum) {
    auto result = field(args, name);
    validateText(result, name, maximum);
    return result;
}

Seconds nextScheduledRun(std::string_view expression, std::string_view timezone, Seconds after) {
    const auto next = Cron(expression, timezone).next(after);
    if (!next) throw Conflict("Cron has no future occurrence in the supported date range");
    return *next;
}

bool validExternalId(std::string_view id) {
    return !id.empty() && id.size() <= 256 && std::ranges::none_of(id, [](unsigned char c) { return c < 32 || c == 127; });
}

Json route(const Intake& intake) {
    if (!validBotId(intake.botId) || intake.environmentId.empty() || intake.projectId.empty() || !intake.config.is_object())
        throw Conflict("Automation requires a configured Bot");
    modelCallLimit(intake.config);
    return {{"environmentId", intake.environmentId}, {"projectId", intake.projectId}, {"config", intake.config}};
}

Intake intake(std::string_view bot, std::string_view request, const Json& routing) {
    return {std::string(bot), std::string(request), field(routing, "environmentId"), field(routing, "projectId"), routing.at("config")};
}

struct Automation {
    Json definition;
    std::optional<Seconds> next;
};

Automation automation(const Json& args, Seconds now) {
    if (!args.is_object()) throw Conflict("Invalid automation arguments");
    const auto kind = field(args, "kind");
    Json def = {{"kind", kind}, {"request", requiredText(args, "request", maxRequestBytes)}};
    if (kind == "trigger") {
        const auto source = field(args, "source");
        if (!validBotId(source)) throw Conflict("Invalid webhook source");
        const auto filter = args.value("eventFilter", Json::object());
        if (!filter.is_object() || filter.dump().size() > 8192) throw Conflict("Invalid eventFilter");
        def["source"] = source;
        def["eventFilter"] = filter;
        return {std::move(def), std::nullopt};
    }
    if (kind != "schedule" && kind != "reminder") throw Conflict("Unknown automation kind");
    if (now < 0 || now > lastAutomationTime) throw Conflict("Clock is outside the supported range");
    const auto timezone = requiredText(args, "timezone", 128);
    def["timezone"] = timezone;
    if (kind == "reminder") {
        validateTimezone(timezone);
        const auto fireAt = requiredInteger(args, "fireAt");
        if (fireAt <= now || fireAt > lastAutomationTime) throw Conflict("fireAt must be in the future");
        def["fireAt"] = fireAt;
        if (args.contains("label")) def["label"] = requiredText(args, "label", 200);
        return {std::move(def), fireAt};
    }
    const auto expression = requiredText(args, "cron", 256);
    def["cron"] = expression;
    const auto next = nextScheduledRun(expression, timezone, now);
    return {std::move(def), next};
}

Json automationView(const Statement& row) {
    auto item = row.json(2);
    item["id"] = row.number(0);
    item["botId"] = row.string(1);
    item["status"] = row.string(3);
    item["nextRunAt"] = row.number(4) ? Json(row.number(4)) : Json(nullptr);
    item["createdBy"] = row.string(5);
    item["createdAt"] = row.number(6);
    if (!row.string(7).empty()) item["configId"] = row.string(7);
    if (!row.string(8).empty()) item["error"] = row.string(8);
    return item;
}

bool matchesEvent(const Json& filter, const Json& event, const SecretRedactor& scrub) {
    for (const auto& [name, value] : filter.items()) {
        const auto actual = event.find(name);
        if (actual == event.end() || *actual != value || scrub.text(name) != name || scrub.payload(*actual) != *actual) return false;
    }
    return true;
}
}

void Store::Impl::initializeAutomationSchema() {
    if (sqlite3_exec(db, R"SQL(
CREATE TABLE IF NOT EXISTS automations(
  id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, kind TEXT NOT NULL CHECK(kind IN ('schedule','reminder','trigger')),
  definition TEXT NOT NULL, routing TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('active','inactive','canceled','delivered','failed')),
  next_run_at INTEGER, created_by TEXT NOT NULL, created_at INTEGER NOT NULL, config_id TEXT, error TEXT,
  UNIQUE(bot_id,config_id));
CREATE INDEX IF NOT EXISTS automation_due ON automations(next_run_at) WHERE status='active' AND next_run_at IS NOT NULL;
CREATE INDEX IF NOT EXISTS automation_bot ON automations(bot_id,id DESC);
CREATE INDEX IF NOT EXISTS automation_active_bot ON automations(bot_id,id DESC) WHERE status='active';
CREATE INDEX IF NOT EXISTS automation_source ON automations(json_extract(definition,'$.source'))
  WHERE status='active' AND kind='trigger';
CREATE TABLE IF NOT EXISTS automation_firings(
  dedupe_key TEXT NOT NULL, bot_id TEXT NOT NULL, fired_at INTEGER NOT NULL, receipts TEXT NOT NULL,
  PRIMARY KEY(dedupe_key,bot_id));
)SQL",
                     nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}

Json Store::createAutomation(const Person& actor, const Intake& target, const Json& args, Seconds now) {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    if (!policy().allows(actor, Action::bot_decide, target.botId)) throw AccessDenied("Automation access denied");
    auto result = p_->createAutomation(target, args, "person:" + actor.id, now);
    tx.commit();
    return result;
}

Json Store::Impl::createAutomation(const Intake& target, const Json& args, std::string_view author, Seconds now) {
    auto item = automation(args, now);
    const auto routing = route(target);
    const auto scrub = item.definition.at("kind") == "trigger" ? sharedRedactor() : redactor(target.botId);
    scrub.requirePublic({target.botId, routing, author, item.definition});
    if (item.definition.contains("eventFilter") && scrub.payload(item.definition["eventFilter"]) != item.definition["eventFilter"])
        throw Conflict("Secret values cannot be used as event filters");
    exec(R"SQL(
INSERT INTO automations(bot_id,kind,definition,routing,status,next_run_at,created_by,created_at)
VALUES(?,?,?,?,'active',?,?,?)
)SQL",
         {target.botId, item.definition.at("kind"), item.definition, routing, item.next ? Json(*item.next) : Json(nullptr), author, now});
    return {{"id", last()}, {"status", "active"}, {"nextRunAt", item.next ? Json(*item.next) : Json(nullptr)}};
}

Json Store::automations(const Person& actor, std::string_view bot, Id before, bool activeOnly) const {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db, Transaction::Mode::read);
    if (!Policy(*p_).allows(actor, Action::bot_view, bot)) throw AccessDenied("Automation access denied");
    auto result = p_->automations(bot, before, activeOnly);
    tx.commit();
    return result;
}

Json Store::Impl::automations(std::string_view bot, Id before, bool activeOnly) const {
    const auto sql = std::string{R"SQL(
SELECT id,bot_id,definition,status,next_run_at,created_by,created_at,config_id,error
FROM automations WHERE bot_id=? AND (?=0 OR id<?)
)SQL"} + (activeOnly ? "AND status='active' " : "") +
                     "ORDER BY id DESC LIMIT 21";
    auto rows = query(sql.c_str(), {bot, before, before});
    Json items = Json::array();
    Id next = 0;
    const auto scrub = redactor(bot);
    std::optional<SecretRedactor> shared;
    while (rows.next()) {
        if (items.size() == 20) {
            next = items.back().at("id");
            break;
        }
        auto item = automationView(rows);
        if (item.at("kind") == "trigger") {
            if (!shared) shared = sharedRedactor();
            auto filter = shared->payload(item.at("eventFilter"));
            item.erase("eventFilter");
            item = shared->record(std::move(item));
            item["eventFilter"] = std::move(filter);
        } else
            item = scrub.record(std::move(item));
        items.push_back(std::move(item));
    }
    return {{"automations", std::move(items)}, {"before", next}};
}

Json Store::activeConfiguredSchedules(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT config_id,definition FROM automations
WHERE bot_id=? AND config_id IS NOT NULL AND status='active'
)SQL",
                          {bot});
    auto schedules = Json::object();
    while (rows.next()) schedules[rows.string(0)] = rows.json(1);
    return schedules;
}

Json Store::cancelAutomation(const Person& actor, std::string_view bot, Id id) {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    if (!policy().allows(actor, Action::bot_decide, bot)) throw AccessDenied("Automation access denied");
    auto result = p_->cancelAutomation(bot, id);
    tx.commit();
    return result;
}

Json Store::Impl::cancelAutomation(std::string_view bot, Id id) {
    auto row = query("SELECT status FROM automations WHERE bot_id=? AND id=?", {bot, id});
    if (!row.next()) throw Conflict("Automation not found");
    if (row.string(0) == "delivered") return {{"id", id}, {"status", "delivered"}};
    exec("UPDATE automations SET status='canceled',next_run_at=NULL,error=NULL WHERE id=?", {id});
    return {{"id", id}, {"status", "canceled"}};
}

std::vector<ScheduleImportError> Store::syncSchedules(std::string_view bot, const std::vector<ConfiguredSchedule>& schedules, Seconds now) {
    return syncSchedules(bot, now, [&](const auto& commit) { commit(schedules, [] {}); });
}

std::vector<ScheduleImportError> Store::syncSchedules(std::string_view bot, Seconds now, const EditConfiguredSchedules& edit) {
    if (now < 0 || now > lastAutomationTime) throw Conflict("Clock is outside the supported range");
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    std::vector<ScheduleImportError> errors;
    edit([&](const auto& schedules, const auto& publish) {
        errors = p_->stageConfiguredSchedules(bot, schedules, now);
        publish();
        tx.commit();
    });
    return errors;
}

std::vector<ScheduleImportError> Store::Impl::stageConfiguredSchedules(std::string_view bot,
                                                                       const std::vector<ConfiguredSchedule>& schedules, Seconds now) {
    if (botDeleted(bot)) return {};
    const auto scrub = redactor(bot);
    std::map<std::string_view, std::size_t> counts;
    for (const auto& schedule : schedules) ++counts[schedule.id];

    std::set<std::string> configured;
    std::vector<ScheduleImportError> errors;
    for (const auto& schedule : schedules) {
        try {
            if (!validBotId(schedule.id) || schedule.routing.botId != bot) throw Conflict("Invalid configured schedule");
            if (counts.at(schedule.id) > 1) throw Conflict("Duplicate configured schedule ID");
            syncSchedule(schedule, now, scrub);
            configured.insert(schedule.id);
        } catch (const Conflict& error) {
            errors.push_back({scrub.text(schedule.id), error.what()});
        }
    }
    exec(R"SQL(
UPDATE automations SET status='inactive',next_run_at=NULL,error=NULL WHERE bot_id=? AND status IN ('active','failed','delivered')
AND config_id IS NOT NULL AND config_id NOT IN (SELECT value FROM json_each(?))
)SQL",
         {bot, Json(configured)});
    return errors;
}

void Store::Impl::syncSchedule(const ConfiguredSchedule& schedule, Seconds now, const SecretRedactor& scrub) {
    const auto& bot = schedule.routing.botId;
    validateText(schedule.routing.request, "request", maxRequestBytes);
    validateText(schedule.expression, "cron", 256);
    validateText(schedule.timezone, "timezone", 128);
    const auto routing = route(schedule.routing);
    const Json def = {
        {"kind", "schedule"}, {"request", schedule.routing.request}, {"cron", schedule.expression}, {"timezone", schedule.timezone}};
    scrub.requirePublic({bot, schedule.id, routing, def});
    const auto after = now > 0 ? now - 1 : now;
    auto old = query(R"SQL(
SELECT id,definition,status,routing FROM automations WHERE bot_id=? AND config_id=?
)SQL",
                     {bot, schedule.id});
    if (!old.next()) {
        const auto next = nextScheduledRun(schedule.expression, schedule.timezone, after);
        exec(R"SQL(
INSERT INTO automations(bot_id,kind,definition,routing,status,next_run_at,created_by,created_at,config_id)
VALUES(?,'schedule',?,?,'active',?,'config',?,?)
)SQL",
             {bot, def, routing, next, now, schedule.id});
        return;
    }

    const auto oldDef = old.json(1);
    const auto status = old.string(2);
    const bool timingChanged = oldDef.at("cron") != def.at("cron") || oldDef.at("timezone") != def.at("timezone");
    if (status != "canceled" && (timingChanged || status == "inactive" || status == "failed")) {
        const auto next = nextScheduledRun(schedule.expression, schedule.timezone, after);
        exec("UPDATE automations SET definition=?,routing=?,status='active',next_run_at=?,error=NULL WHERE id=?",
             {def, routing, next, old.number(0)});
    } else if (oldDef != def || old.json(3) != routing) {
        if (timingChanged) nextScheduledRun(schedule.expression, schedule.timezone, after);
        exec("UPDATE automations SET definition=?,routing=? WHERE id=?", {def, routing, old.number(0)});
    }
}

void Store::pruneAutomations(const std::vector<std::string>& configuredBots) {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    auto absent = p_->query(R"SQL(
SELECT json_group_array(DISTINCT bot_id) FROM automations
WHERE status IN ('active','failed') AND bot_id NOT IN (SELECT value FROM json_each(?))
)SQL",
                            {Json(configuredBots).dump()});
    absent.next();
    p_->retireAutomations(absent.json(0));
    tx.commit();
}

void Store::Impl::retireAutomations(const Json& botIds) {
    exec(R"SQL(
UPDATE automations SET status=CASE WHEN config_id IS NULL THEN 'canceled' ELSE 'inactive' END,
  next_run_at=NULL,error=NULL
WHERE status IN ('active','failed') AND bot_id IN (SELECT value FROM json_each(?))
)SQL",
         Json::array({botIds}));
}

Json Store::Impl::automationTool(std::string_view bot, const Person& actor, const Json& routing, const Decision& decision, Seconds now) {
    const auto& name = decision.tool;
    const auto& args = decision.args;
    if (name == "list_schedules") return automations(bot, args.contains("before") ? requiredInteger(args, "before") : 0);
    if (name == "remove_schedule") {
        if (!Policy(*this).allows(actor, Action::bot_settings, bot) || Policy(*this).forbidden(bot, Action::automation_change, bot))
            throw AccessDenied("Automation change denied");
        return cancelAutomation(bot, requiredInteger(args, "id"));
    }
    std::string_view kind;
    if (name == "create_schedule")
        kind = "schedule";
    else if (name == "create_reminder")
        kind = "reminder";
    else if (name == "create_trigger")
        kind = "trigger";
    else
        return nullptr;
    if (!Policy(*this).allows(actor, Action::bot_settings, bot) || Policy(*this).forbidden(bot, Action::automation_change, bot))
        throw AccessDenied("Automation change denied");
    auto definition = args;
    definition["kind"] = kind;
    return createAutomation(intake(bot, field(args, "request"), routing), definition, "bot:" + std::string(bot), now);
}

void Store::fireDueAutomations(Seconds now) {
    p_->fireDueAutomations(now);
}

void Store::Impl::fireDueAutomations(Seconds now) {
    if (now < 0 || now > lastAutomationTime) throw Conflict("Clock is outside the supported range");
    std::lock_guard lock(mutex);
    for (const auto id : dueAutomations(now)) {
        Transaction tx(db);
        processDueAutomation(id, now);
        tx.commit();
    }
}

void Store::Impl::skipDueAutomations(std::string_view bot, Seconds now) {
    for (const auto id : dueAutomations(now, bot)) processDueAutomation(id, now, true);
}

std::vector<Id> Store::Impl::dueAutomations(Seconds now, std::string_view bot) const {
    std::vector<Id> due;
    auto rows = query(R"SQL(
SELECT id FROM automations WHERE status='active' AND next_run_at<=? AND (?2='' OR bot_id=?2)
ORDER BY next_run_at,id
)SQL",
                      {now, bot});
    while (rows.next()) due.push_back(rows.number(0));
    return due;
}

void Store::Impl::applyPlannerModel(Json& config, const PlannerModel& planner) {
    config["model"] = planner.model;
    config["modelProvider"] = planner.provider;
    if (planner.reasoningEffort)
        config["reasoningEffort"] = *planner.reasoningEffort;
    else
        config.erase("reasoningEffort");
}

Json Store::Impl::automationRoute(std::string_view bot, Json routing) const {
    if (!automationModel) return routing;
    if (const auto current = automationModel(bot)) {
        auto& config = routing.at("config");
        applyPlannerModel(config, *current);
        if (current->modelCallLimit)
            config["modelCallLimit"] = *current->modelCallLimit;
        else
            config.erase("modelCallLimit");
    }
    return routing;
}

void Store::Impl::processDueAutomation(Id id, Seconds now, bool skip) {
    auto row = query(R"SQL(
SELECT bot_id,definition,routing,next_run_at
FROM automations WHERE id=? AND status='active' AND next_run_at<=?
)SQL",
                     {id, now});
    if (!row.next()) return;
    const auto bot = row.string(0);
    const bool skipped = skip || botPaused(bot);
    try {
        const auto def = row.json(1);
        const auto kind = field(def, "kind");
        const auto from = row.number(3);
        auto occurrences = Cron::Occurrences{from, from, 1};
        std::optional<Seconds> next;
        if (kind == "schedule") {
            const Cron cron(field(def, "cron"), field(def, "timezone"));
            next = cron.next(now);
            if (!skipped) occurrences = cron.between(from, now);
        }
        if (!skipped && occurrences.count) {
            const auto key = "schedule:" + std::to_string(id) + ":" + std::to_string(occurrences.last);
            if (!query("SELECT 1 FROM automation_firings WHERE dedupe_key=? AND bot_id=?", {key, bot}).next()) {
                Json trigger = {{"triggerKind", kind},
                                {"triggerId", id},
                                {"targetTime", occurrences.last},
                                {"coveredFrom", occurrences.first},
                                {"coveredTo", occurrences.last},
                                {"occurrenceCount", occurrences.count},
                                {"firedAt", now},
                                {"timezone", def.at("timezone")}};
                if (def.contains("label")) trigger["label"] = def.at("label");
                auto scrub = redactor(bot);
                trigger = scrub.record(std::move(trigger));
                const auto routing = automationRoute(bot, row.json(2));
                const auto task = submit(intake(bot, field(def, "request"), routing), 0, nullptr, TaskSource::request, trigger);
                const auto receipts = Json::array({{{"taskId", task}, {"botId", bot}, {"triggerId", id}}});
                exec("INSERT INTO automation_firings VALUES(?,?,?,?)", {key, bot, now, receipts});
            }
        }
        const auto finished = skipped ? "canceled" : "delivered";
        exec("UPDATE automations SET next_run_at=?,status=? WHERE id=?",
             {next ? Json(*next) : Json(nullptr), next ? "active" : finished, id});
    } catch (const Conflict& error) {
        exec("UPDATE automations SET status='failed',next_run_at=NULL,error=? WHERE id=?", {error.what(), id});
    }
}

Json Store::receiveWebhook(const Person& actor, std::string_view source, std::string_view eventId, const Json& event, Seconds now) {
    if (!validBotId(source) || !validExternalId(eventId) || !event.is_object() || event.dump().size() > 16000)
        throw Conflict("Invalid webhook event");
    const auto key = "webhook:" + std::string(source) + ":" + std::string(eventId);
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    const auto permits = [&](std::string_view bot) { return policy().allows(actor, Action::bot_decide, bot); };
    std::map<std::string, Json> previous;
    auto existing = p_->query("SELECT bot_id,receipts FROM automation_firings WHERE dedupe_key=?", {key});
    while (existing.next()) {
        const auto bot = existing.string(0);
        if (permits(bot)) previous.emplace(bot, existing.json(1));
    }

    auto rows = p_->query(R"SQL(
SELECT id,bot_id,definition,routing FROM automations WHERE status='active' AND kind='trigger'
AND json_extract(definition,'$.source')=? ORDER BY id
)SQL",
                          {source});
    const auto scrub = p_->sharedRedactor();
    std::optional<Json> payload;
    std::map<std::string, Json> received;
    while (rows.next()) {
        const auto bot = rows.string(1);
        if (previous.contains(bot) || !permits(bot)) continue;
        auto& receipts = received.try_emplace(bot, Json::array()).first->second;
        const auto def = rows.json(2);
        if (receipts.is_null() || !matchesEvent(def.at("eventFilter"), event, scrub)) continue;
        if (p_->botPaused(bot)) {
            receipts = nullptr;
            continue;
        }
        const auto routing = p_->automationRoute(bot, rows.json(3));
        const Json destination = {bot, routing};
        if (scrub.record(destination, SecretRedactor::Fields::external) != destination) continue;
        const auto request = bounded(scrub.text(field(def, "request")), maxRequestBytes);
        if (request.find_first_not_of(" \r\n\t") == request.npos) continue;
        if (!payload) payload = scrub.payload(event);
        const auto id = rows.number(0);
        const Json trigger = {{"triggerKind", "webhook"},
                              {"triggerId", id},
                              {"source", scrub.text(std::string(source))},
                              {"event", *payload},
                              {"firedAt", now}};
        const auto task = p_->submit(intake(bot, request, routing), 0, nullptr, TaskSource::request, trigger);
        receipts.push_back({{"taskId", task}, {"botId", bot}, {"triggerId", id}});
    }
    if (previous.empty() && received.empty()) throw AccessDenied("Webhook source unavailable");
    Json result{{"duplicate", received.empty()}, {"tasks", Json::array()}};
    auto& tasks = result.at("tasks");
    Json skippedBots = Json::array();
    const auto append = [&](const std::string& bot, const Json& receipts) {
        if (receipts.is_null())
            skippedBots.push_back(scrub.text(bot));
        else
            tasks.insert(tasks.end(), receipts.begin(), receipts.end());
    };
    for (const auto& [bot, receipts] : received) {
        p_->exec("INSERT INTO automation_firings VALUES(?,?,?,?)", {key, bot, now, receipts.dump()});
        append(bot, receipts);
    }
    for (const auto& [bot, receipts] : previous) append(bot, receipts);
    if (!skippedBots.empty()) {
        result["message"] =
            skippedBots.size() == 1 ? "This event was skipped because the Bot was paused." : "Paused Bots skipped this event.";
        result["skippedBots"] = std::move(skippedBots);
    }
    tx.commit();
    return result;
}
}
