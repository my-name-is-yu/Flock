#include "storage.hpp"
#include "date/tz.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>

namespace flockv0 {
namespace {
int number(std::string_view value) {
    int result = 0;
    const auto [end,error] = std::from_chars(value.data(),value.data()+value.size(),result);
    if (value.empty() || error != std::errc{} || end != value.data()+value.size())
        throw Conflict("Invalid number in cron expression");
    return result;
}

std::bitset<60> cronField(std::string_view input, int low, int high, bool weekday = false) {
    std::bitset<60> values;
    for (;;) {
        const auto comma = input.find(',');
        const auto entry = input.substr(0,comma);
        const auto slash = entry.find('/');
        const auto range = entry.substr(0,slash);
        const int step = slash == std::string_view::npos ? 1 : number(entry.substr(slash+1));
        int first = low, last = high;
        if (range != "*") {
            const auto dash = range.find('-');
            first = number(range.substr(0,dash));
            last = dash != std::string_view::npos ? number(range.substr(dash+1)) :
                (slash != std::string_view::npos ? high : first);
        }
        if (first < low || last > high || first > last || step < 1 || step > high - low + 1)
            throw Conflict("Cron field is outside its allowed range");
        for (int n = first; n <= last; n += step) values.set(weekday && n == 7 ? 0 : n);
        if (comma == std::string_view::npos) return values;
        input.remove_prefix(comma+1);
    }
}

const date::time_zone* timeZone(std::string_view name) {
    try { return date::locate_zone(name); }
    catch (const std::exception&) { throw Conflict("Unknown IANA timezone: " + std::string(name)); }
}
}
Cron::Cron(std::string_view expression, std::string_view timezone) {
    std::istringstream fields{std::string(expression)};
    std::array<std::string,5> value;
    for (auto& field : value)
        if (!(fields >> field)) throw Conflict("Cron requires five fields");
    std::string extra;
    if (fields >> extra) throw Conflict("Cron requires five fields");
    minutes_ = cronField(value[0],0,59);
    hours_ = cronField(value[1],0,23);
    days_ = cronField(value[2],1,31);
    months_ = cronField(value[3],1,12);
    weekdays_ = cronField(value[4],0,7,true);
    anyDay_ = value[2].starts_with("*");
    anyWeekday_ = value[4].starts_with("*");
    zone_ = timeZone(timezone);
}

bool Cron::matches(Seconds utc) const {
    using namespace std::chrono;
    const auto local = zone_->to_local(date::sys_seconds{seconds{utc}});
    const auto day = date::floor<date::days>(local);
    const date::year_month_day ymd{day};
    const auto time = date::make_time(date::floor<minutes>(local - day));
    const auto weekday = date::weekday{day}.c_encoding();
    return hours_.test(time.hours().count()) && minutes_.test(time.minutes().count()) &&
        matchesDay(unsigned(ymd.month()),unsigned(ymd.day()),weekday);
}

bool Cron::matchesDay(unsigned month, unsigned day, unsigned weekday) const {
    const bool dom = days_.test(day), dow = weekdays_.test(weekday);
    return months_.test(month) && (anyDay_ || anyWeekday_ ? dom && dow : dom || dow);
}

Seconds Cron::next(Seconds after) const {
    using namespace std::chrono;
    auto cursor = date::floor<minutes>(date::sys_seconds{seconds{after}}) + minutes{1};
    const auto limit = cursor + date::days{8 * 366 + 1};
    while (cursor < limit) {
        const auto offset = zone_->get_info(cursor);
        const auto shift = date::floor<minutes>(offset.offset);
        const auto end = std::min(date::ceil<minutes>(offset.end),limit);
        auto local = date::local_time<minutes>{cursor.time_since_epoch() + shift};
        const auto localEnd = date::local_time<minutes>{end.time_since_epoch() + shift};
        while (local < localEnd) {
            const auto day = date::floor<date::days>(local);
            const date::year_month_day ymd{day};
            if (matchesDay(unsigned(ymd.month()),unsigned(ymd.day()),date::weekday{day}.c_encoding())) {
                const auto first = (local - day).count();
                for (auto hour = first / 60; hour < 24; ++hour) {
                    if (!hours_.test(hour)) continue;
                    for (auto minute = hour == first / 60 ? first % 60 : 0; minute < 60; ++minute) {
                        if (!minutes_.test(minute)) continue;
                        const auto candidate = day + minutes{hour*60+minute};
                        if (candidate < localEnd) return duration_cast<seconds>(candidate.time_since_epoch() - shift).count();
                        break;
                    }
                }
            }
            local = day + date::days{1};
        }
        cursor = end;
    }
    throw Conflict("Cron has no occurrence in the next eight years");
}

namespace {
constexpr Seconds lastSupportedTime = 253402300799;

std::string requiredText(const Json& args, std::string_view name, std::size_t maximum) {
    auto result = field(args,name);
    if (result.size() > maximum || result.find_first_not_of(" \r\n\t") == result.npos)
        throw Conflict("Invalid " + std::string(name));
    return result;
}

bool validExternalId(std::string_view id) {
    return !id.empty() && id.size() <= 256 && std::ranges::none_of(id,[](unsigned char c) { return c < 32 || c == 127; });
}

Json route(const Intake& intake) {
    if (!validBotId(intake.botId) || intake.environmentId.empty() || intake.projectId.empty() || !intake.config.is_object())
        throw Conflict("Automation requires a configured Bot");
    modelCallLimit(intake.config);
    return {{"environmentId",intake.environmentId},{"projectId",intake.projectId},{"config",intake.config}};
}

Intake intake(std::string_view bot, std::string_view request, const Json& routing) {
    return {std::string(bot),std::string(request),field(routing,"environmentId"),
            field(routing,"projectId"),routing.at("config")};
}

struct Automation {
    Json definition;
    Seconds next = 0;
};

Automation automation(const Json& args, Seconds now) {
    if (!args.is_object()) throw Conflict("Invalid automation arguments");
    const auto kind = field(args,"kind");
    Json def = {{"kind",kind},{"request",requiredText(args,"request",16000)}};
    if (kind == "trigger") {
        const auto source = field(args,"source");
        if (!validBotId(source)) throw Conflict("Invalid webhook source");
        const auto filter = args.value("eventFilter",Json::object());
        if (!filter.is_object() || filter.dump().size() > 8192) throw Conflict("Invalid eventFilter");
        def["source"] = source;
        def["eventFilter"] = filter;
        return {std::move(def),0};
    }
    if (kind != "schedule" && kind != "reminder") throw Conflict("Unknown automation kind");
    if (now < 0 || now > lastSupportedTime) throw Conflict("Clock is outside the supported range");
    const auto timezone = requiredText(args,"timezone",128);
    def["timezone"] = timezone;
    if (kind == "reminder") {
        timeZone(timezone);
        const auto fireAt = requiredInteger(args,"fireAt");
        if (fireAt > lastSupportedTime) throw Conflict("Invalid fireAt");
        def["fireAt"] = fireAt;
        if (args.contains("label")) def["label"] = requiredText(args,"label",200);
        return {std::move(def),fireAt};
    }
    const auto expression = requiredText(args,"cron",256);
    def["cron"] = expression;
    return {std::move(def),Cron(expression,timezone).next(now)};
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
    return item;
}

bool matchesEvent(const Json& filter, const Json& event) {
    for (const auto& [name,value] : filter.items()) {
        const auto actual = event.find(name);
        if (actual == event.end() || *actual != value) return false;
    }
    return true;
}
}

void Store::Impl::migrateAutomation() {
    Transaction tx(db);
    if (sqlite3_exec(db,R"SQL(
CREATE TABLE IF NOT EXISTS automations(
  id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, kind TEXT NOT NULL CHECK(kind IN ('schedule','reminder','trigger')),
  definition TEXT NOT NULL, routing TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('active','canceled','delivered')),
  next_run_at INTEGER, created_by TEXT NOT NULL, created_at INTEGER NOT NULL, config_id TEXT,
  UNIQUE(bot_id,config_id));
CREATE INDEX IF NOT EXISTS automation_due ON automations(next_run_at) WHERE status='active' AND next_run_at IS NOT NULL;
CREATE INDEX IF NOT EXISTS automation_bot ON automations(bot_id,id DESC);
CREATE INDEX IF NOT EXISTS automation_source ON automations(json_extract(definition,'$.source'))
  WHERE status='active' AND kind='trigger';
CREATE TABLE IF NOT EXISTS automation_firings(
  dedupe_key TEXT PRIMARY KEY, fired_at INTEGER NOT NULL, receipts TEXT NOT NULL);
)SQL",nullptr,nullptr,nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    tx.commit();
}

Json Store::createAutomation(const Person& actor, const Intake& target, const Json& args, Seconds now) {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    if (!access().permitsPerson(actor,target.botId,Role::decide)) throw AccessDenied("Automation access denied");
    auto result = p_->createAutomation(target,args,"person:" + actor.id,now);
    tx.commit();
    return result;
}

Json Store::Impl::createAutomation(const Intake& target, const Json& args, std::string_view author, Seconds now) {
    auto item = automation(args,now);
    const auto routing = route(target);
    const auto scrub = redactor(target.botId);
    scrub.requirePublic({target.botId,routing,author,item.definition});
    if (item.definition.contains("eventFilter") && scrub.payload(item.definition["eventFilter"]) != item.definition["eventFilter"])
        throw Conflict("Secret values cannot be used as event filters");
    exec(R"SQL(
INSERT INTO automations(bot_id,kind,definition,routing,status,next_run_at,created_by,created_at)
VALUES(?,?,?,?,'active',?,?,?)
)SQL",{target.botId,item.definition.at("kind"),item.definition,routing,
       item.next ? Json(item.next) : Json(nullptr),author,now});
    return {{"id",last()},{"status","active"},{"nextRunAt",item.next ? Json(item.next) : Json(nullptr)}};
}

Json Store::automations(const Person& actor, std::string_view bot, Id before) const {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db,Transaction::Mode::read);
    if (!Access(*p_).permitsPerson(actor,bot,Role::view)) throw AccessDenied("Automation access denied");
    auto result = p_->automations(bot,before);
    tx.commit();
    return result;
}

Json Store::Impl::automations(std::string_view bot, Id before) const {
    auto rows = query(R"SQL(
SELECT id,bot_id,definition,status,next_run_at,created_by,created_at,config_id
FROM automations WHERE bot_id=? AND (?=0 OR id<?) ORDER BY id DESC LIMIT 21
)SQL",{bot,before,before});
    Json items = Json::array();
    Id next = 0;
    while (rows.next()) {
        if (items.size() == 20) {
            next = items.back().at("id");
            break;
        }
        items.push_back(automationView(rows));
    }
    return redactor(bot).payload({{"automations",items},{"before",next}});
}

Json Store::cancelAutomation(const Person& actor, std::string_view bot, Id id) {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    if (!access().permitsPerson(actor,bot,Role::decide)) throw AccessDenied("Automation access denied");
    auto result = p_->cancelAutomation(bot,id);
    tx.commit();
    return result;
}

Json Store::Impl::cancelAutomation(std::string_view bot, Id id) {
    auto row = query("SELECT status FROM automations WHERE bot_id=? AND id=?",{bot,id});
    if (!row.next()) throw Conflict("Automation not found");
    const auto status = row.string(0);
    if (status == "active")
        exec("UPDATE automations SET status='canceled',next_run_at=NULL WHERE id=?",{id});
    return {{"id",id},{"status",status == "active" ? "canceled" : status}};
}

void Store::syncSchedules(std::string_view bot, const std::vector<ConfiguredSchedule>& schedules, Seconds now) {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    std::vector<std::string> configured;
    for (const auto& schedule : schedules) {
        if (!validBotId(schedule.id) || schedule.routing.botId != bot ||
            std::ranges::find(configured,schedule.id) != configured.end()) throw Conflict("Invalid configured schedule");
        configured.push_back(schedule.id);
        const auto routing = route(schedule.routing);
        auto old = p_->query("SELECT definition,routing FROM automations WHERE bot_id=? AND config_id=?", {bot,schedule.id});
        const Json def = {{"kind","schedule"},{"request",schedule.routing.request},
                          {"cron",schedule.expression},{"timezone",schedule.timezone}};
        const bool existing = old.next();
        if (existing && old.json(0) == def && old.json(1) == routing) continue;
        const auto item = automation(def,now > 0 ? now - 1 : now);
        p_->redactor(bot).requirePublic({bot,schedule.id,routing,def});
        p_->exec(R"SQL(
INSERT INTO automations(bot_id,kind,definition,routing,status,next_run_at,created_by,created_at,config_id)
VALUES(?,'schedule',?,?,'active',?,'config',?,?)
ON CONFLICT(bot_id,config_id) DO UPDATE SET definition=excluded.definition,routing=excluded.routing,
  next_run_at=CASE WHEN status='active' AND (
    json_extract(definition,'$.cron')<>json_extract(excluded.definition,'$.cron') OR
    json_extract(definition,'$.timezone')<>json_extract(excluded.definition,'$.timezone'))
    THEN excluded.next_run_at ELSE next_run_at END
)SQL",{bot,item.definition,routing,item.next,now,schedule.id});
    }
    p_->exec(R"SQL(
UPDATE automations SET status='canceled',next_run_at=NULL WHERE bot_id=? AND status='active'
AND config_id IS NOT NULL AND config_id NOT IN (SELECT value FROM json_each(?))
)SQL",{bot,Json(configured)});
    tx.commit();
}

void Store::pruneSchedules(const std::vector<std::string>& configuredBots) {
    std::lock_guard lock(p_->mutex);
    p_->exec(R"SQL(
UPDATE automations SET status='canceled',next_run_at=NULL WHERE status='active'
AND config_id IS NOT NULL AND bot_id NOT IN (SELECT value FROM json_each(?))
)SQL",{Json(configuredBots).dump()});
}

Json Store::Impl::automationTool(std::string_view bot, const Json& routing, const Decision& decision, Seconds now) {
    const auto& name = decision.tool;
    const auto& args = decision.args;
    if (name == "list_schedules") return automations(bot,args.contains("before") ? requiredInteger(args,"before") : 0);
    if (name == "remove_schedule") return cancelAutomation(bot,requiredInteger(args,"id"));
    std::string_view kind;
    if (name == "create_schedule") kind = "schedule";
    else if (name == "create_reminder") kind = "reminder";
    else if (name == "create_trigger") kind = "trigger";
    else return nullptr;
    auto definition = args;
    definition["kind"] = kind;
    return createAutomation(intake(bot,field(args,"request"),routing),definition,"bot:" + std::string(bot),now);
}

void Store::fireDueAutomations(Seconds now) { p_->fireDueAutomations(now); }

void Store::Impl::fireDueAutomations(Seconds now) {
    std::lock_guard lock(mutex);
    if (!query("SELECT 1 FROM automations WHERE status='active' AND next_run_at<=? LIMIT 1",{now}).next()) return;
    Transaction tx(db);
    std::vector<Id> due;
    {
        auto rows = query("SELECT id FROM automations WHERE status='active' AND next_run_at<=? ORDER BY next_run_at,id",{now});
        while (rows.next()) due.push_back(rows.number(0));
    }
    for (const auto id : due) {
        auto row = query("SELECT bot_id,definition,routing,next_run_at FROM automations WHERE id=?",{id});
        if (!row.next()) throw std::runtime_error("Automation disappeared");
        const auto bot = row.string(0);
        const auto def = row.json(1);
        const auto kind = field(def,"kind");
        const auto from = row.number(3);
        Seconds to = from, next = 0;
        Id count = 1;
        if (kind == "schedule") {
            const Cron cron(field(def,"cron"),field(def,"timezone"));
            for (next = cron.next(to); next <= now; next = cron.next(to)) {
                to = next;
                ++count;
            }
        }
        const auto key = "schedule:" + std::to_string(id) + ":" + std::to_string(to);
        exec("INSERT INTO automation_firings VALUES(?,?,'[]') ON CONFLICT(dedupe_key) DO NOTHING",{key,now});
        if (sqlite3_changes(db)) {
            Json trigger = {{"triggerKind",kind},{"triggerId",id},{"targetTime",to},{"coveredFrom",from},
                {"coveredTo",to},{"occurrenceCount",count},{"firedAt",now},{"timezone",def.at("timezone")}};
            if (def.contains("label")) trigger["label"] = def.at("label");
            const auto task = submit(intake(bot,field(def,"request"),row.json(2)),{},0,nullptr,trigger);
            exec("UPDATE automation_firings SET receipts=? WHERE dedupe_key=?",
                 {Json::array({{{"taskId",task},{"botId",bot},{"trigger",trigger}}}),key});
        }
        exec("UPDATE automations SET next_run_at=?,status=? WHERE id=?",
             {next ? Json(next) : Json(nullptr),next ? "active" : "delivered",id});
    }
    tx.commit();
}

Json Store::receiveWebhook(const Person& actor, std::string_view source, std::string_view eventId,
                           const Json& event, Seconds now) {
    if (!validBotId(source) || !validExternalId(eventId) || !event.is_object() || event.dump().size() > 16000)
        throw Conflict("Invalid webhook event");
    const auto key = "webhook:" + std::string(source) + ":" + std::string(eventId);
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    const auto authorize = [&](std::string_view bot) {
        if (!access().permitsPerson(actor,bot,Role::decide)) throw AccessDenied("Webhook access denied");
        p_->redactor(bot).requirePublic({source,eventId});
    };
    auto existing = p_->query("SELECT receipts FROM automation_firings WHERE dedupe_key=?",{key});
    if (existing.next()) {
        const auto saved = existing.json(0);
        for (const auto& bot : saved.at("bots")) authorize(bot.get_ref<const std::string&>());
        tx.commit();
        return {{"duplicate",true},{"tasks",saved.at("tasks")}};
    }

    auto rows = p_->query(R"SQL(
SELECT id,bot_id,definition,routing FROM automations WHERE status='active' AND kind='trigger'
AND json_extract(definition,'$.source')=? ORDER BY id
)SQL",{source});
    std::vector<std::string> targets;
    Json tasks = Json::array();
    while (rows.next()) {
        const auto bot = rows.string(1);
        authorize(bot);
        if (std::ranges::find(targets,bot) == targets.end()) targets.push_back(bot);
        const auto def = rows.json(2);
        if (!matchesEvent(def.at("eventFilter"),event)) continue;
        const auto payload = p_->redactor(bot).payload(event);
        const Json trigger = {{"triggerKind","webhook"},{"triggerId",rows.number(0)},{"source",source},
                              {"externalEventId",eventId},{"event",payload},{"firedAt",now}};
        const auto task = p_->submit(intake(bot,field(def,"request"),rows.json(3)),{},0,nullptr,trigger);
        tasks.push_back({{"taskId",task},{"botId",bot},{"triggerId",rows.number(0)}});
    }
    if (targets.empty()) throw AccessDenied("Webhook source unavailable");
    p_->exec("INSERT INTO automation_firings VALUES(?,?,?)",{key,now,Json{{"bots",targets},{"tasks",tasks}}});
    tx.commit();
    return {{"duplicate",false},{"tasks",tasks}};
}
}
