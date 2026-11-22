#include "bots.hpp"
#include "flock/bot_profile.hpp"

#include "date/tz.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

namespace flockv0::app {
namespace {
constexpr std::size_t maxConfigBytes = 128 * 1024;

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

void writeAtomic(const std::filesystem::path& file, std::string_view content) {
    static std::atomic<std::uint64_t> counter{0};
    const auto temp = file.parent_path() / (file.filename().string() + ".tmp." + std::to_string(++counter));
    {
        std::ofstream output(temp,std::ios::binary | std::ios::trunc);
        if (!output) throw Conflict("Unable to write " + file.filename().string());
        output.write(content.data(),static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            std::error_code ignore;
            std::filesystem::remove(temp,ignore);
            throw Conflict("Unable to write " + file.filename().string());
        }
    }
    std::error_code error;
    std::filesystem::rename(temp,file,error);
    if (error) {
        std::error_code ignore;
        std::filesystem::remove(temp,ignore);
        throw Conflict("Unable to save " + file.filename().string());
    }
}

bool present(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") != std::string::npos;
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first,last - first + 1));
}

std::string hashHex(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : bytes) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string out(16,'0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = hex[hash & 0xF];
        hash >>= 4;
    }
    return out;
}

std::string sanitizeFilename(std::string_view raw) {
    const auto slash = raw.find_last_of("/\\");
    if (slash != std::string_view::npos) raw.remove_prefix(slash + 1);
    std::string cleaned;
    cleaned.reserve(std::min<std::size_t>(raw.size(),120));
    for (const unsigned char ch : raw) {
        if (ch < 32 || ch == 127) continue;
        cleaned.push_back(static_cast<char>(ch));
    }
    return bounded(trim(cleaned),120);
}

std::string validatedBotName(const Json& profile) {
    auto name = botProfileName(profile.value("name",Json()));
    if (!name) throw Conflict("Bot name must be 1 to 32 characters and contain no control characters");
    return std::move(*name);
}

std::string slugForName(std::string_view name) {
    std::string slug;
    bool dash = false;
    for (const unsigned char ch : name) {
        if (std::isalnum(ch)) {
            slug.push_back(static_cast<char>(std::tolower(ch)));
            dash = false;
        } else if (!slug.empty() && !dash) {
            slug.push_back('-');
            dash = true;
        }
        if (slug.size() >= 40) break;
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty() || !validId(slug)) return "bot";
    return slug;
}

std::string roleStarterSoul(std::string_view role) {
    if (role == "reviewer")
        return "Review changes carefully, verify results with evidence, and highlight risks clearly.\n";
    if (role == "builder")
        return "Implement requested changes cleanly, run relevant tests, and report verified outcomes.\n";
    if (role == "researcher")
        return "Investigate questions thoroughly, gather concrete evidence, and summarize findings clearly.\n";
    return {};
}

const avatar::ImageFormat* storedFormat(const Json& source) {
    if (!source.is_object() || source.value("kind",Json()) != "upload" ||
        !source.contains("file") || !source["file"].is_string()) return nullptr;
    for (const auto& format : avatar::imageFormats)
        if (source["file"] == format.filename) return &format;
    return nullptr;
}

struct ResolvedImage {
    const avatar::ImageFormat* format;
    std::string bytes;
};

std::optional<ResolvedImage> resolveImage(const std::filesystem::path& directory, const Json& source) {
    const auto* format = storedFormat(source);
    if (!format) return std::nullopt;
    const auto target = directory / format->filename;
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(target,error))) return std::nullopt;
    try {
        auto bytes = readText(target,avatar::maxImageBytes);
        if (avatar::imageFormat(bytes) == format) return ResolvedImage{format,std::move(bytes)};
    } catch (const Conflict&) {}
    return std::nullopt;
}

void removeReplacedAvatarFile(const std::filesystem::path& directory, const Json& previous, std::string_view keep) {
    const auto* format = storedFormat(previous);
    if (!format || format->filename == keep) return;
    std::error_code ignore;
    std::filesystem::remove(directory / format->filename,ignore);
}

Json prepareAvatar(const Json& profile, const std::filesystem::path& botDir, const Json& existingAvatar,
                   const std::optional<AvatarImage>& image, std::string& keepFile, std::string& writeFile,
                   std::string_view& writeBytes) {
    auto result = avatar::appearance(profile.value("avatar",Json()));
    if (result.is_null()) throw Conflict("Invalid avatar");
    if (result["kind"] != "upload") return result;

    const auto* format = image ? avatar::imageFormat(image->bytes) : nullptr;
    if (image) {
        if (!format) throw Conflict("Choose a PNG, JPEG, WebP or GIF up to 5 MiB");
        writeFile = format->filename;
        writeBytes = image->bytes;
    } else {
        const auto existing = resolveImage(botDir,existingAvatar);
        if (!existing) throw Conflict("Uploaded image is required");
        format = existing->format;
        if (!result.contains("filename") && existingAvatar.contains("filename"))
            result["filename"] = existingAvatar["filename"];
    }
    keepFile = format->filename;
    result["file"] = format->filename;
    if (result.contains("filename")) {
        auto filename = result["filename"].is_string() ? sanitizeFilename(result["filename"].get_ref<const std::string&>()) :
            std::string{};
        if (filename.empty()) result.erase("filename");
        else result["filename"] = std::move(filename);
    }
    return result;
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

std::vector<Schedule> parseSchedules(const Json& config) {
    std::vector<Schedule> parsed;
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
        parsed.push_back({scheduleId,expression,timezone,request,Cron(expression,timezone)});
    }
    return parsed;
}

}

Cron::Cron(std::string expression, std::string timezone) {
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
    try { zone_ = date::locate_zone(timezone); }
    catch (const std::exception&) { throw Conflict("Unknown IANA timezone: " + timezone); }
}

bool Cron::matches(Seconds utc) const {
    using namespace std::chrono;
    const auto local = zone_->to_local(date::sys_seconds{seconds{utc}});
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
    auto bot = load(id,false);
    if (!bot) throw Conflict("Bot has no execution settings: " + std::string(id));
    return std::move(*bot);
}

std::optional<Intake> Bots::configuredIntake(std::string_view id, std::string_view request) const {
    const auto bot = load(id,false);
    if (!bot) return std::nullopt;
    return bot->intake(request);
}

std::optional<Bot> Bots::load(std::string_view id, bool scheduledOnly) const {
    const auto file = path(id) / "config.json";
    const auto config = Json::parse(readText(file,maxConfigBytes),nullptr,false);
    if (!config.is_object()) throw Conflict("Invalid Bot config.json");
    auto schedules = parseSchedules(config);
    if (scheduledOnly && schedules.empty()) return std::nullopt;
    if (schedules.empty() && !config.contains("environmentId") && !config.contains("projectId") &&
        !config.contains("modelProvider") && !config.contains("model")) return std::nullopt;
    Bot bot;
    bot.id = id;
    bot.schedules = std::move(schedules);
    bot.name = config.contains("name") ? validatedString(config,"name") : bot.id;
    bot.environmentId = validatedString(config,"environmentId");
    bot.projectId = validatedString(config,"projectId");
    if (!plainId(bot.environmentId) || !plainId(bot.projectId))
        throw Conflict("Use canonical environmentId and projectId identifiers from T3");
    const auto provider = validatedString(config,"modelProvider");
    if (provider != "chatgpt") throw Conflict("Unsupported modelProvider: " + provider);
    bot.settings = {{"modelProvider",provider},{"model",validatedString(config,"model")}};
    if (config.contains("modelCallLimit")) bot.settings["modelCallLimit"] = modelCallLimit(config);
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

Json Bots::cardLocked(std::string_view id) const {
    const auto botDir = path(id);
    const auto config = Json::parse(readText(botDir / "config.json",maxConfigBytes),nullptr,false);
    if (!config.is_object())
        return {{"id",id},{"name",id},{"soul","Check this Bot’s configuration."},{"cron",Json::array()}};

    std::string name(id);
    if (config.contains("name") && config["name"].is_string() && present(config["name"].get_ref<const std::string&>()))
        name = config["name"].get<std::string>();

    auto schedules = Json::array();
    try {
        for (const auto& schedule : parseSchedules(config))
            schedules.push_back({{"id",schedule.id},{"schedule",schedule.expression},
                {"timezone",schedule.timezone},{"request",schedule.request}});
    } catch (const Conflict&) {
        // Keep the profile available for editing while the invalid schedule set is disabled.
    }

    Json item = {{"id",id},{"name",name},{"soul",readText(botDir / "SOUL.md",8192,true)},{"cron",schedules}};
    if (config.contains("role") && avatar::validRole(config["role"]))
        item["role"] = config["role"];
    auto appearance = avatar::appearance(config.value("avatar",Json()));
    if (!appearance.is_null()) {
        if (const auto image = resolveImage(botDir,config["avatar"]))
            appearance["version"] = hashHex(image->bytes);
        item["avatar"] = std::move(appearance);
    }
    return item;
}

Json Bots::cards() const {
    std::lock_guard lock(mutex_);
    auto items = Json::array();
    for (const auto& id : ids()) {
        try {
            items.push_back(cardLocked(id));
        } catch (const std::exception&) {
            items.push_back({{"id",id},{"name",id},{"soul","Check this Bot’s configuration."},{"cron",Json::array()}});
        }
    }
    return items;
}

Json Bots::create(const Json& profile, const std::optional<AvatarImage>& image) {
    if (!profile.is_object()) throw Conflict("Profile must be an object");
    const auto name = validatedBotName(profile);
    std::string role = "custom";
    if (profile.contains("role")) {
        if (!avatar::validRole(profile["role"]))
            throw Conflict("Unsupported bot role");
        role = profile["role"].get<std::string>();
    }
    std::string keepFile, writeFile;
    std::string_view writeBytes;
    const auto avatar = prepareAvatar(profile,{},Json{},image,keepFile,writeFile,writeBytes);

    std::lock_guard lock(mutex_);
    std::filesystem::create_directories(directory_);
    const auto base = slugForName(name);
    std::string id = base;
    for (unsigned suffix = 2; std::filesystem::exists(directory_ / id); ++suffix)
        id = base + "-" + std::to_string(suffix);
    const auto botDir = directory_ / id;
    std::filesystem::create_directories(botDir);
    try {
        if (!writeFile.empty()) writeAtomic(botDir / writeFile,writeBytes);
        Json config = {{"name",name},{"role",role},{"avatar",avatar}};
        writeAtomic(botDir / "config.json",config.dump(2,' ',false,Json::error_handler_t::replace) + "\n");
        const auto starter = roleStarterSoul(role);
        if (!starter.empty()) writeAtomic(botDir / "SOUL.md",starter);
        return cardLocked(id);
    } catch (...) {
        std::error_code ignore;
        std::filesystem::remove_all(botDir,ignore);
        throw;
    }
}

Json Bots::updateProfile(std::string_view id, const Json& profile, const std::optional<AvatarImage>& image) {
    if (!profile.is_object()) throw Conflict("Profile must be an object");
    const auto name = validatedBotName(profile);

    std::lock_guard lock(mutex_);
    const auto botDir = path(id);
    const auto file = botDir / "config.json";
    auto config = Json::parse(readText(file,maxConfigBytes),nullptr,false);
    if (!config.is_object()) throw Conflict("Invalid Bot config.json");

    std::string keepFile, writeFile;
    std::string_view writeBytes;
    const auto existingAvatar = config.contains("avatar") ? config["avatar"] : Json{};
    const bool changeAvatar = profile.contains("avatar");
    config["name"] = name;
    if (changeAvatar)
        config["avatar"] = prepareAvatar(profile,botDir,existingAvatar,image,keepFile,writeFile,writeBytes);

    // Check the size the readers accept before anything on disk changes.
    auto text = config.dump(2,' ',false,Json::error_handler_t::replace) + "\n";
    if (text.size() > maxConfigBytes) text = config.dump(-1,' ',false,Json::error_handler_t::replace) + "\n";
    if (text.size() > maxConfigBytes) throw Conflict("Bot config.json would exceed its size limit");

    if (!writeFile.empty()) writeAtomic(botDir / writeFile,writeBytes);
    writeAtomic(file,text);
    if (changeAvatar) removeReplacedAvatarFile(botDir,existingAvatar,keepFile);
    return cardLocked(id);
}

std::optional<AvatarImage> Bots::avatarImage(std::string_view id) const {
    std::lock_guard lock(mutex_);
    try {
        const auto botDir = path(id);
        const auto config = Json::parse(readText(botDir / "config.json",maxConfigBytes),nullptr,false);
        const auto source = config.value("avatar",Json());
        if (!avatar::appearance(source).is_null()) {
            if (auto image = resolveImage(botDir,source))
                return AvatarImage{std::string(image->format->contentType),std::move(image->bytes)};
        }
        return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

Scheduler::Scheduler(const Bots& bots, Store& store, Seconds startedAt, ErrorReporter report)
    : bots_(bots), store_(store),
      next_(std::chrono::ceil<std::chrono::minutes>(std::chrono::seconds{startedAt}).count()*60), report_(std::move(report)) {}

void Scheduler::tick(Seconds now) {
    if (next_ > now) return;
    std::vector<Bot> configured;
    for (const auto& id : bots_.ids()) {
        try {
            if (auto bot = bots_.load(id,true)) configured.push_back(std::move(*bot));
        }
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
}
