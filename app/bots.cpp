#include "bots.hpp"

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
constexpr std::size_t maxAvatarBytes = 5 * 1024 * 1024;
constexpr std::size_t maxConfigBytes = 128 * 1024;
constexpr std::array<std::string_view,4> allowedAvatarFiles = {
    "avatar.png", "avatar.jpg", "avatar.webp", "avatar.gif"
};
constexpr std::array<std::string_view,7> allowedShapes = {
    "mug", "sprout", "controller", "lightbulb", "chat", "watering-can", "barbell"
};
constexpr std::array<std::string_view,8> allowedIcons = {
    "star", "orbit", "zap", "compass", "feather", "hexagon", "telescope", "sparkles"
};
constexpr std::array<std::string_view,4> allowedRoles = {
    "reviewer", "builder", "researcher", "custom"
};

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

std::size_t utf8Codepoints(std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char ch : text)
        if ((ch & 0xC0) != 0x80) ++count;
    return count;
}

bool validHexColor(std::string_view color) {
    if (color.size() != 4 && color.size() != 7) return false;
    if (color[0] != '#') return false;
    return std::all_of(color.begin() + 1,color.end(),[](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

template <std::size_t N>
bool inList(const std::array<std::string_view,N>& list, std::string_view value) {
    return std::find(list.begin(),list.end(),value) != list.end();
}

struct DetectedImage {
    std::string contentType;
    std::string filename;
};

std::optional<DetectedImage> detectImage(std::string_view bytes) {
    if (bytes.size() < 8 || bytes.size() > maxAvatarBytes) return std::nullopt;
    const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(bytes[index]); };
    if (byte(0) == 0x89 && byte(1) == 'P' && byte(2) == 'N' && byte(3) == 'G' &&
        byte(4) == 0x0D && byte(5) == 0x0A && byte(6) == 0x1A && byte(7) == 0x0A)
        return DetectedImage{"image/png","avatar.png"};
    if (byte(0) == 0xFF && byte(1) == 0xD8 && byte(2) == 0xFF)
        return DetectedImage{"image/jpeg","avatar.jpg"};
    if (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a"))
        return DetectedImage{"image/gif","avatar.gif"};
    if (bytes.size() >= 12 && bytes.substr(0,4) == "RIFF" && bytes.substr(8,4) == "WEBP")
        return DetectedImage{"image/webp","avatar.webp"};
    return std::nullopt;
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
    const auto found = profile.find("name");
    if (found == profile.end() || !found->is_string()) throw Conflict("Bot name must be a string");
    const auto name = trim(found->get_ref<const std::string&>());
    if (name.empty() || name.size() > 128 || utf8Codepoints(name) > 32)
        throw Conflict("Bot name must be 1 to 32 characters");
    if (std::any_of(name.begin(),name.end(),[](unsigned char ch) { return ch < 32 || ch == 127; }))
        throw Conflict("Bot name contains invalid characters");
    return name;
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

// Removes only the image the previous avatar referenced, so files the editor never wrote survive.
void removeReplacedAvatarFile(const std::filesystem::path& directory, const Json& previous, std::string_view keep) {
    if (!previous.is_object() || previous.value("kind","") != "upload") return;
    const auto file = previous.find("file");
    if (file == previous.end() || !file->is_string()) return;
    const auto& name = file->get_ref<const std::string&>();
    if (name == keep || !inList(allowedAvatarFiles,name)) return;
    std::error_code ignore;
    std::filesystem::remove(directory / name,ignore);
}

Json prepareAvatar(const Json& profile, const std::filesystem::path& botDir, const Json& existingAvatar,
                   const std::optional<AvatarImage>& image, std::string& keepFile, std::string& writeFile,
                   std::string_view& writeBytes) {
    const auto found = profile.find("avatar");
    if (found == profile.end() || !found->is_object()) throw Conflict("Avatar must be an object");
    const auto& source = *found;
    const auto kindIt = source.find("kind");
    if (kindIt == source.end() || !kindIt->is_string()) throw Conflict("Avatar kind is required");
    const auto& kind = kindIt->get_ref<const std::string&>();
    std::string color = "#69c9ac";
    if (source.contains("color")) {
        if (!source["color"].is_string() || !validHexColor(source["color"].get_ref<const std::string&>()))
            throw Conflict("Avatar color must be a hex color");
        color = source["color"].get<std::string>();
    }
    if (kind == "shape") {
        const auto exampleIt = source.find("example");
        if (exampleIt == source.end() || !exampleIt->is_string() ||
            !inList(allowedShapes,exampleIt->get_ref<const std::string&>()))
            throw Conflict("Unsupported avatar shape");
        keepFile.clear();
        return {{"kind","shape"},{"example",exampleIt->get<std::string>()},{"color",color}};
    }
    if (kind == "icon") {
        const auto iconIt = source.find("icon");
        if (iconIt == source.end() || !iconIt->is_string() ||
            !inList(allowedIcons,iconIt->get_ref<const std::string&>()))
            throw Conflict("Unsupported avatar icon");
        keepFile.clear();
        return {{"kind","icon"},{"icon",iconIt->get<std::string>()},{"color",color}};
    }
    if (kind == "upload") {
        if (image) {
            const auto detected = detectImage(image->bytes);
            if (!detected) throw Conflict("Choose a PNG, JPEG, WebP or GIF up to 5 MB");
            keepFile = detected->filename;
            writeFile = detected->filename;
            writeBytes = image->bytes;
            const auto filename = source.contains("filename") && source["filename"].is_string()
                ? sanitizeFilename(source["filename"].get_ref<const std::string&>()) : std::string{};
            Json stored = {{"kind","upload"},{"file",detected->filename},
                           {"version",hashHex(image->bytes)},{"color",color}};
            if (!filename.empty()) stored["filename"] = filename;
            return stored;
        }
        if (existingAvatar.is_object() && existingAvatar.value("kind","") == "upload" &&
            existingAvatar.contains("file") && existingAvatar["file"].is_string()) {
            const auto existingFile = existingAvatar["file"].get<std::string>();
            const auto target = botDir / existingFile;
            if (inList(allowedAvatarFiles,existingFile) &&
                std::filesystem::is_regular_file(target) && !std::filesystem::is_symlink(target)) {
                keepFile = existingFile;
                Json stored = {{"kind","upload"},{"file",existingFile},
                               {"version",existingAvatar.value("version","1")},{"color",color}};
                const auto filename = source.contains("filename") && source["filename"].is_string()
                    ? sanitizeFilename(source["filename"].get_ref<const std::string&>())
                    : sanitizeFilename(existingAvatar.value("filename",""));
                if (!filename.empty()) stored["filename"] = filename;
                return stored;
            }
        }
        throw Conflict("Uploaded image is required");
    }
    throw Conflict("Unsupported avatar kind");
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
}

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
    const auto config = Json::parse(readText(file,maxConfigBytes),nullptr,false);
    if (!config.is_object()) throw Conflict("Invalid Bot config.json");
    Bot bot;
    bot.id = id;
    bot.name = config.contains("name") ? validatedString(config,"name") : bot.id;
    bot.environmentId = validatedString(config,"environmentId");
    bot.projectId = validatedString(config,"projectId");
    if (!plainId(bot.environmentId) || !plainId(bot.projectId))
        throw Conflict("Use canonical environmentId and projectId identifiers from T3");
    bot.settings = {{"modelProvider",validatedString(config,"modelProvider")},{"model",validatedString(config,"model")}};
    if (config.contains("enforceAccess")) bot.settings["enforceAccess"] = config.at("enforceAccess").get<bool>();
    for (const auto key : {"services","mcpServers"}) {
        if (!config.contains(key)) continue;
        if (!config[key].is_array()) throw Conflict("Connection names must be an array");
        for (const auto& name : config[key])
            if (!name.is_string() || !plainId(name.get<std::string>())) throw Conflict("Invalid connection name");
        bot.settings[key] = config[key];
    }
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

Json Bots::cardLocked(std::string_view id) const {
    const auto botDir = path(id);
    const auto config = Json::parse(readText(botDir / "config.json",maxConfigBytes),nullptr,false);
    if (!config.is_object())
        return {{"id",id},{"name",id},{"soul","Check this Bot’s configuration."},{"cron",Json::array()}};

    std::string name(id);
    if (config.contains("name") && config["name"].is_string() && present(config["name"].get_ref<const std::string&>()))
        name = config["name"].get<std::string>();

    auto schedules = Json::array();
    if (config.contains("cron") && config["cron"].is_array()) {
        for (const auto& schedule : config["cron"]) {
            if (!schedule.is_object()) continue;
            try {
                const auto scheduleId = validatedString(schedule,"id");
                const auto expression = validatedString(schedule,"schedule");
                const auto timezone = validatedString(schedule,"timezone");
                const auto request = validatedString(schedule,"request");
                schedules.push_back({{"id",scheduleId},{"schedule",expression},
                    {"timezone",timezone},{"request",request}});
            } catch (const Conflict&) {}
        }
    }

    Json item = {{"id",id},{"name",name},{"soul",readText(botDir / "SOUL.md",8192,true)},{"cron",schedules}};
    if (config.contains("role") && config["role"].is_string() && inList(allowedRoles,config["role"].get_ref<const std::string&>()))
        item["role"] = config["role"];
    if (config.contains("avatar") && (config["avatar"].is_string() || config["avatar"].is_object())) {
        auto avatar = config["avatar"];
        if (avatar.is_object() && avatar.value("kind","") == "upload") {
            const auto file = avatar.value("file",std::string{});
            const auto target = botDir / file;
            if (!inList(allowedAvatarFiles,file) ||
                !std::filesystem::is_regular_file(target) || std::filesystem::is_symlink(target)) {
                avatar.erase("file");
                avatar.erase("version");
            }
        }
        item["avatar"] = std::move(avatar);
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
        if (!profile["role"].is_string() || !inList(allowedRoles,profile["role"].get_ref<const std::string&>()))
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
        if (!config.is_object() || !config.contains("avatar") || !config["avatar"].is_object())
            return std::nullopt;
        const auto& avatar = config["avatar"];
        if (avatar.value("kind","") != "upload" || !avatar.contains("file") || !avatar["file"].is_string())
            return std::nullopt;
        const auto file = avatar["file"].get<std::string>();
        if (!inList(allowedAvatarFiles,file)) return std::nullopt;
        const auto target = botDir / file;
        if (!std::filesystem::is_regular_file(target) || std::filesystem::is_symlink(target))
            return std::nullopt;
        const auto bytes = readText(target,maxAvatarBytes);
        const auto detected = detectImage(bytes);
        if (!detected) return std::nullopt;
        return AvatarImage{detected->contentType,bytes};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool Bots::hasCron(std::string_view id) const {
    try {
        const auto config = Json::parse(readText(path(id) / "config.json",maxConfigBytes),nullptr,false);
        if (!config.is_object()) return true;
        return config.contains("cron") && (!config["cron"].is_array() || !config["cron"].empty());
    } catch (const std::exception&) {
        return true;
    }
}

Scheduler::Scheduler(const Bots& bots, Store& store, Seconds startedAt, ErrorReporter report)
    : bots_(bots), store_(store),
      next_(std::chrono::ceil<std::chrono::minutes>(std::chrono::seconds{startedAt}).count()*60), report_(std::move(report)) {}

void Scheduler::tick(Seconds now) {
    if (next_ > now) return;
    std::vector<Bot> configured;
    for (const auto& id : bots_.ids()) {
        if (!bots_.hasCron(id)) continue;
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
}
