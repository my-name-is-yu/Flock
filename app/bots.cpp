#include "bots.hpp"

#include "date/tz.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

namespace flockv0::app {
namespace {
bool validId(std::string_view id) {
    const auto starts = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    return !id.empty() && id.size() <= 128 && std::string_view(starts).find(id.front()) != std::string_view::npos &&
        id.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_.") == std::string_view::npos;
}

bool plainId(std::string_view id) {
    return !id.empty() && id.size() <= 256 &&
        std::none_of(id.begin(),id.end(),[](unsigned char ch) { return ch <= 32 || ch == 127; });
}

std::string readText(const std::filesystem::path& file, std::size_t limit, bool optional = false) {
    std::ifstream input(file,std::ios::binary);
    if (!input) {
        if (optional && !std::filesystem::exists(file)) return {};
        throw Conflict("Unable to read " + file.filename().string());
    }
    std::string out(limit + 1,'\0');
    input.read(out.data(),static_cast<std::streamsize>(out.size()));
    if (input.bad()) throw Conflict("Unable to read " + file.filename().string());
    out.resize(static_cast<std::size_t>(input.gcount()));
    if (out.size() > limit) {
        if (!optional) throw Conflict(file.filename().string() + " exceeds the configured size limit");
        out = bounded(out,limit);
    }
    return out;
}

bool present(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") != std::string::npos;
}

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

std::string validatedString(const Json& object, const char* key) {
    const auto value = object.find(key);
    if (value == object.end() || !value->is_string() || !present(value->get_ref<const std::string&>()))
        throw Conflict(std::string(key) + " must be a nonempty string");
    return value->get<std::string>();
}
} // namespace

Cron::Cron(std::string expression, std::string timezone) : timezone_(std::move(timezone)) {
    std::istringstream fields(expression);
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
    try { date::locate_zone(timezone_); }
    catch (const std::exception&) { throw Conflict("Unknown IANA timezone: " + timezone_); }
}

bool Cron::matches(Seconds utc) const {
    using namespace std::chrono;
    const auto local = date::locate_zone(timezone_)->to_local(date::sys_seconds{seconds{utc}});
    const auto day = date::floor<date::days>(local);
    const date::year_month_day ymd{day};
    const auto time = date::make_time(date::floor<minutes>(local - day));
    const auto weekday = date::weekday{day}.c_encoding();
    if (!months_.test(unsigned(ymd.month())) || !hours_.test(time.hours().count()) || !minutes_.test(time.minutes().count()))
        return false;
    const bool dom = days_.test(unsigned(ymd.day())), dow = weekdays_.test(weekday);
    return anyDay_ || anyWeekday_ ? dom && dow : dom || dow;
}

Intake Bot::intake(std::string_view request) const {
    return {id,std::string(request),environmentId,projectId,settings,{},0};
}

Bots::Bots(std::filesystem::path directory) : directory_(std::move(directory)) {}

std::filesystem::path Bots::path(std::string_view id) const {
    if (!validId(id)) throw Conflict("Invalid Bot ID");
    const auto bot = directory_ / id;
    if (!std::filesystem::is_directory(bot) || std::filesystem::is_symlink(bot))
        throw Conflict("Bot does not exist: " + std::string(id));
    return bot;
}

std::vector<std::string> Bots::ids() const {
    std::vector<std::string> result;
    if (!std::filesystem::exists(directory_)) return result;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        const auto id = entry.path().filename().string();
        if (validId(id) && entry.is_directory() && !entry.is_symlink() &&
            std::filesystem::is_regular_file(entry.path() / "config.json")) result.push_back(id);
    }
    std::sort(result.begin(),result.end());
    return result;
}

Bot Bots::get(std::string_view id) const {
    const auto file = path(id) / "config.json";
    const auto config = Json::parse(readText(file,128*1024),nullptr,false);
    if (!config.is_object()) throw Conflict("Invalid Bot config.json");
    Bot bot;
    bot.id = id;
    bot.name = config.contains("name") ? validatedString(config,"name") : bot.id;
    bot.environmentId = validatedString(config,"environmentId");
    bot.projectId = validatedString(config,"projectId");
    if (!plainId(bot.environmentId) || !plainId(bot.projectId))
        throw Conflict("Use canonical environmentId and projectId identifiers from T3");
    bot.settings = {{"modelProvider",validatedString(config,"modelProvider")},{"model",validatedString(config,"model")}};
    if (config.contains("conversationEnvironments")) {
        const auto& destinations = config["conversationEnvironments"];
        if (!destinations.is_object() || destinations.size() > 16)
            throw Conflict("conversationEnvironments must map up to 16 names to T3 environment IDs");
        auto& saved = bot.settings["conversationEnvironments"] = Json::object();
        for (auto alias = destinations.begin(); alias != destinations.end(); ++alias) {
            const auto environment = validatedString(destinations,alias.key().c_str());
            if (alias.key().empty() || alias.key().size() > 80 || !plainId(environment))
                throw Conflict("Use short conversation environment names and canonical T3 environment IDs");
            saved[alias.key()] = environment;
        }
    }
    const auto timeout = config.value("runTimeoutSeconds",Json(1800));
    if (!timeout.is_number_integer() || timeout.get<long double>() < 1 || timeout.get<long double>() > 2147483647)
        throw Conflict("runTimeoutSeconds must be a positive integer");
    bot.settings["runTimeoutSeconds"] = timeout.get<int>();
    if (config.contains("session")) {
        const auto& session = config["session"];
        if (!session.is_object()) throw Conflict("session must be an object");
        auto& stored = bot.settings["session"] = Json::object();
        for (const auto* key : {"model","providerInstanceId","runtimeMode"})
            if (session.contains(key)) stored[key] = validatedString(session,key);
        if (session.contains("workspaceStrategy")) {
            if (!session["workspaceStrategy"].is_object()) throw Conflict("session.workspaceStrategy must be an object");
            stored["workspaceStrategy"] = session["workspaceStrategy"];
        }
    }
    if (config.contains("avatar")) bot.avatar = config["avatar"];
    const auto allowedBots = config.value("allowedBots",Json::array());
    if (!allowedBots.is_array()) throw Conflict("allowedBots must be an array of Bot IDs");
    std::set<std::string> allowedIds;
    for (const auto& item : allowedBots) {
        if (!item.is_string()) throw Conflict("allowedBots must contain only Bot IDs");
        const auto& peerId = item.get_ref<const std::string&>();
        if (!validId(peerId) || peerId == id || !allowedIds.insert(peerId).second)
            throw Conflict("allowedBots must contain valid, unique IDs of other Bots");
        bot.allowedBots.push_back(peerId);
    }
    const auto schedules = config.value("cron",Json::array());
    if (!schedules.is_array()) throw Conflict("cron must be an array");
    std::set<std::string> scheduleIds;
    for (const auto& schedule : schedules) {
        const auto scheduleId = validatedString(schedule,"id");
        const auto expression = validatedString(schedule,"schedule");
        const auto timezone = validatedString(schedule,"timezone");
        const auto request = validatedString(schedule,"request");
        if (!validId(scheduleId) || !scheduleIds.insert(scheduleId).second || request.size() > 16000)
            throw Conflict("Cron IDs must be valid and unique, and requests must fit in 16,000 bytes");
        bot.schedules.push_back({scheduleId,expression,timezone,request,Cron(expression,timezone)});
    }
    return bot;
}

BotNotes Bots::notes(std::string_view id) const {
    const auto bot = path(id);
    return {readText(bot / "SOUL.md",8192,true),readText(bot / "MEMORY.md",8192,true)};
}

std::vector<PeerBot> Bots::peers(std::string_view id) const {
    const auto tryGetConfiguredBot = [this](std::string_view botId) -> std::optional<Bot> {
        try {
            return get(botId);
        } catch (const Conflict&) {
            return std::nullopt;
        } catch (const std::filesystem::filesystem_error&) {
            return std::nullopt;
        }
    };
    const auto sender = tryGetConfiguredBot(id);
    if (!sender) return {};
    std::vector<PeerBot> result;
    for (const auto& peerId : sender->allowedBots) {
        if (const auto peer = tryGetConfiguredBot(peerId)) result.push_back({peer->name,peer->intake("")});
    }
    return result;
}

Json Bots::cards() const {
    auto items = Json::array();
    for (const auto& id : ids()) {
        try {
            const auto bot = get(id);
            auto schedules = Json::array();
            for (const auto& schedule : bot.schedules)
                schedules.push_back({{"id",schedule.id},{"schedule",schedule.expression},
                    {"timezone",schedule.timezone},{"request",schedule.request}});
            Json item = {{"id",id},{"name",bot.name},{"soul",notes(id).soul},{"cron",schedules}};
            if (!bot.avatar.is_null()) item["avatar"] = bot.avatar;
            items.push_back(std::move(item));
        } catch (const std::exception&) {
            items.push_back({{"id",id},{"name",id},{"soul","Check this Bot’s configuration."},{"cron",Json::array()}});
        }
    }
    return items;
}

Scheduler::Scheduler(const Bots& bots, Store& store, Seconds startedAt, ErrorReporter report)
    : bots_(bots), store_(store),
      next_(std::chrono::ceil<std::chrono::minutes>(std::chrono::seconds{startedAt}).count()*60), report_(std::move(report)) {}

void Scheduler::tick(Seconds now) {
    if (next_ > now) return;
    std::vector<Bot> configured;
    for (const auto& id : bots_.ids()) {
        try { configured.push_back(bots_.get(id)); }
        catch (const std::exception& error) { report_(id,error.what()); }
    }
    for (; next_ <= now; next_ += 60) {
        for (const auto& bot : configured) {
            for (const auto& schedule : bot.schedules) {
                if (!schedule.cron.matches(next_)) continue;
                auto item = bot.intake(schedule.request);
                item.scheduleId = schedule.id;
                item.scheduledAt = next_;
                store_.submit(item);
            }
        }
    }
}
} // namespace flockv0::app
