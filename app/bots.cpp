#include "bots.hpp"
#include "flock/session_settings.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <set>

namespace flockv0::app {
namespace {
constexpr std::size_t maxConfigBytes = 128 * 1024;

std::string readUpTo(const std::filesystem::path& file, std::size_t bytes, bool optional) {
    std::ifstream input(file,std::ios::binary);
    if (!input) {
        if (optional && !std::filesystem::exists(file)) return {};
        throw Conflict("Unable to read " + file.filename().string());
    }
    std::string out(bytes,'\0');
    input.read(out.data(),static_cast<std::streamsize>(out.size()));
    if (input.bad()) throw Conflict("Unable to read " + file.filename().string());
    out.resize(static_cast<std::size_t>(input.gcount()));
    return out;
}

std::string readText(const std::filesystem::path& file, std::size_t limit, bool optional = false) {
    auto out = readUpTo(file,limit + 1,optional);
    if (out.size() > limit) {
        if (!optional) throw Conflict(file.filename().string() + " exceeds the configured size limit");
        out = bounded(out,limit);
    }
    return out;
}

bool writeNewFile(const std::filesystem::path& path, std::string_view bytes) {
    auto* output = std::fopen(path.c_str(),"wbx");
    if (!output) {
        if (errno == EEXIST) return false;
        throw Conflict("Unable to create " + path.filename().string());
    }
    const auto written = std::fwrite(bytes.data(),1,bytes.size(),output);
    const auto closed = std::fclose(output);
    if (written != bytes.size() || closed != 0) {
        std::error_code ignore;
        std::filesystem::remove(path,ignore);
        throw Conflict("Unable to write " + path.filename().string());
    }
    return true;
}

void writeAtomic(const std::filesystem::path& file, std::string_view content) {
    static std::atomic<std::uint64_t> counter{0};
    std::filesystem::path temp;
    do {
        temp = file.parent_path() / (file.filename().string() + ".tmp." + std::to_string(++counter));
    } while (!writeNewFile(temp,content));
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
    if (slug.empty() || !validBotId(slug)) return "bot";
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

std::string storedAvatarFile(const Json& avatar) {
    const auto kind = avatar.find("kind");
    const auto file = avatar.find("file");
    if (kind == avatar.end() || *kind != "upload" || file == avatar.end() || !file->is_string()) return {};
    const auto& name = file->get_ref<const std::string&>();
    return AvatarImage::isStoredFilename(name) ? name : std::string{};
}

bool isAvatarFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(std::filesystem::symlink_status(path,error));
}

std::string profileText(const Json& config) {
    auto text = config.dump(2,' ',false,Json::error_handler_t::replace) + "\n";
    if (text.size() > maxConfigBytes) text = config.dump(-1,' ',false,Json::error_handler_t::replace) + "\n";
    if (text.size() > maxConfigBytes) throw Conflict("Bot config.json would exceed its size limit");
    return text;
}

void saveProfile(const std::filesystem::path& dir, Json& config, const AvatarImage* upload,
                 std::string_view previousFile = {}) {
    const auto target = dir / "config.json";
    if (!upload || config.at("avatar").at("kind") != "upload") {
        writeAtomic(target,profileText(config));
        return;
    }
    for (unsigned index = 0; ; ++index) {
        const auto name = upload->storageFilename(index);
        if (name == previousFile) continue;
        config["avatar"]["file"] = name;
        const auto text = profileText(config);
        const auto path = dir / name;
        if (!writeNewFile(path,upload->bytes())) continue;
        try {
            writeAtomic(target,text);
        } catch (...) {
            std::error_code ignore;
            std::filesystem::remove(path,ignore);
            throw;
        }
        return;
    }
}

Json prepareAvatar(const Json& profile, const std::filesystem::path& botDir, const Json& existingAvatar,
                   const std::optional<AvatarImage>& image) {
    auto avatar = validatedAvatarStyle(profile);
    if (avatar["kind"] != "upload") return avatar;
    const auto& source = profile.at("avatar");
    const auto label = source.find("filename");
    const bool hasLabel = label != source.end() && label->is_string();
    auto filename = hasLabel ? label->get<std::string>() : std::string{};
    if (image) {
        avatar["version"] = hashHex(image->bytes());
    } else {
        const auto name = storedAvatarFile(existingAvatar);
        const auto target = botDir / name;
        if (name.empty() || !isAvatarFile(target))
            throw Conflict("Uploaded image is required");
        avatar["file"] = name;
        const auto version = existingAvatar.find("version");
        if (version == existingAvatar.end() || !version->is_string() || version->get_ref<const std::string&>().empty())
            throw Conflict("Upload the image again to restore its version");
        avatar["version"] = *version;
        if (!hasLabel)
            filename = existingAvatar.value("filename","");
    }
    filename = sanitizeFilename(filename);
    if (!filename.empty()) avatar["filename"] = std::move(filename);
    return avatar;
}

std::string validatedString(const Json& object, const char* key) {
    const auto value = object.find(key);
    if (value == object.end() || !value->is_string() || !present(value->get_ref<const std::string&>()))
        throw Conflict(std::string(key) + " must be a nonempty string");
    return value->get<std::string>();
}
}

Intake Bot::intake(std::string_view request) const {
    return {id,std::string(request),environmentId,projectId,settings};
}

Bots::Bots(std::filesystem::path directory) : directory_(std::move(directory)) {}

std::filesystem::path Bots::path(std::string_view id) const {
    if (!validBotId(id)) throw Conflict("Invalid Bot ID");
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
        if (validBotId(id) && entry.is_directory() && !entry.is_symlink() &&
            std::filesystem::is_regular_file(entry.path() / "config.json")) result.push_back(id);
    }
    std::sort(result.begin(),result.end());
    return result;
}

namespace {
Json parseBotConfig(std::string_view text) {
    auto config = Json::parse(text,nullptr,false);
    if (!config.is_object()) throw Conflict("Invalid Bot config.json");
    return config;
}

Json readBotConfig(const std::filesystem::path& file) {
    return parseBotConfig(readText(file,maxConfigBytes));
}

Bot configuredBot(std::string_view id, const Json& config, std::vector<std::string>* scheduleErrors = nullptr) {
    Bot bot;
    bot.id = id;
    bot.name = config.contains("name") ? validatedString(config,"name") : bot.id;
    if (config.contains("environmentId")) bot.environmentId = validatedString(config,"environmentId");
    if (config.contains("projectId")) bot.projectId = validatedString(config,"projectId");
    if ((!bot.environmentId.empty() && !validPlainId(bot.environmentId)) ||
        (!bot.projectId.empty() && !validPlainId(bot.projectId)))
        throw Conflict("Use canonical environmentId and projectId identifiers from T3");
    if (!bot.projectId.empty() && bot.environmentId.empty())
        throw Conflict("A pinned T3 project needs its environmentId");
    bot.settings = {{"botDisplayName",bot.name},{"modelProvider",validatedString(config,"modelProvider")},{"model",validatedString(config,"model")}};
    for (const auto key : {"services","mcpServers"}) {
        if (!config.contains(key)) continue;
        if (!config[key].is_array()) throw Conflict("Connection names must be an array");
        for (const auto& name : config[key])
            if (!name.is_string() || !validPlainId(name.get_ref<const std::string&>())) throw Conflict("Invalid connection name");
        bot.settings[key] = config[key];
    }
    if (config.contains("modelCallLimit")) bot.settings["modelCallLimit"] = modelCallLimit(config);
    if (config.contains("conversationEnvironments")) {
        const auto& destinations = config["conversationEnvironments"];
        if (!destinations.is_object())
            throw Conflict("conversationEnvironments must map names to T3 destinations");
        auto& saved = bot.settings["conversationEnvironments"] = Json::object();
        for (auto alias = destinations.begin(); alias != destinations.end(); ++alias) {
            const auto environment = alias->is_object() ? validatedString(*alias,"environmentId")
                : validatedString(destinations,alias.key().c_str());
            if (alias.key().empty() || alias.key().size() > 80 || !validPlainId(environment))
                throw Conflict("Use short conversation environment names and canonical T3 environment IDs");
            if (alias->is_object()) {
                const auto project = validatedString(*alias,"projectId");
                const bool hasGateway = alias->contains("gatewayEnvironmentId");
                const auto gateway = hasGateway ? validatedString(*alias,"gatewayEnvironmentId") : std::string{};
                if (alias->size() != (hasGateway ? 3u : 2u) || !validPlainId(project) ||
                    (hasGateway && !validPlainId(gateway)))
                    throw Conflict("T3 destination objects require canonical environmentId, projectId, and optional gatewayEnvironmentId");
                saved[alias.key()] = {{"environmentId",environment},{"projectId",project}};
                if (hasGateway) saved[alias.key()]["gatewayEnvironmentId"] = gateway;
            } else {
                saved[alias.key()] = environment;
            }
        }
    }
    const auto timeout = config.value("runTimeoutSeconds",Json(1800));
    if (!timeout.is_number_integer() || timeout.get<long double>() < 1 || timeout.get<long double>() > 2147483647)
        throw Conflict("runTimeoutSeconds must be a positive integer");
    bot.settings["runTimeoutSeconds"] = timeout.get<int>();
    if (config.contains("session")) {
        try { bot.settings["session"] = normalizeSessionSettings(config["session"]); }
        catch (const Conflict& error) {
            if (!scheduleErrors) throw;
            scheduleErrors->push_back(error.what());
            // Preserve the cron due time. This snapshot must stop at Session launch validation.
            bot.settings["session"] = nullptr;
        }
    }
    const auto allowedBots = config.value("allowedBots",Json::array());
    if (!allowedBots.is_array()) throw Conflict("allowedBots must be an array of Bot IDs");
    std::set<std::string> allowedIds;
    for (const auto& item : allowedBots) {
        if (!item.is_string()) throw Conflict("allowedBots must contain only Bot IDs");
        const auto& peerId = item.get_ref<const std::string&>();
        if (!validBotId(peerId) || peerId == id || !allowedIds.insert(peerId).second)
            throw Conflict("allowedBots must contain valid, unique IDs of other Bots");
        bot.allowedBots.push_back(peerId);
    }
    if (const auto role = config.find("role"); role != config.end() && role->is_string())
        bot.role = bounded(role->get_ref<const std::string&>(), 80);
    return bot;
}

std::vector<ConfiguredSchedule> configuredSchedules(
        std::string_view id, const Json& config, std::vector<std::string>& errors) {
    std::vector<ConfiguredSchedule> schedules;
    try {
        const auto cron = config.value("cron",Json::array());
        if (!cron.is_array()) throw Conflict("cron must be an array");
        if (!cron.empty()) {
            const auto bot = configuredBot(id,config,&errors);
            if (bot.environmentId.empty() || bot.projectId.empty())
                throw Conflict("Configured schedules need a T3 environment and project");
            std::map<std::string_view,std::size_t> counts;
            for (const auto& entry : cron) {
                const auto key = entry.find("id");
                if (key == entry.end() || !key->is_string()) continue;
                const auto& scheduleId = key->get_ref<const std::string&>();
                if (validBotId(scheduleId)) ++counts[scheduleId];
            }
            for (std::size_t i = 0; i < cron.size(); ++i) {
                const auto& entry = cron[i];
                try {
                    const auto scheduleId = validatedString(entry,"id");
                    if (validBotId(scheduleId) && counts.at(scheduleId) > 1)
                        throw Conflict("Duplicate configured schedule ID");
                    schedules.push_back({scheduleId,validatedString(entry,"schedule"),
                        validatedString(entry,"timezone"),bot.intake(validatedString(entry,"request"))});
                } catch (const Conflict& error) {
                    errors.push_back("cron entry " + std::to_string(i + 1) + ": " + error.what());
                }
            }
        }
    } catch (const Conflict& error) {
        errors.emplace_back(error.what());
    } catch (const Json::exception& error) {
        errors.emplace_back(error.what());
    }
    return schedules;
}

void reportScheduleErrors(std::string_view id, const std::vector<ScheduleImportError>& importErrors,
                          const std::vector<std::string>& configErrors, const Bots::ScheduleReporter& report) {
    for (const auto& error : importErrors) report(id,"cron schedule " + error.id + ": " + error.message);
    for (const auto& error : configErrors) report(id,error);
}
}

Bot Bots::get(std::string_view id) const {
    return configuredBot(id,readBotConfig(path(id) / "config.json"));
}

std::string Bots::soul(std::string_view id) const {
    return readText(path(id) / "SOUL.md",maxBotRoleBytes,true);
}

std::string Bots::initialMemory(std::string_view id) const {
    return readText(path(id) / "MEMORY.md",8192,true);
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
        if (const auto peer = tryGetConfiguredBot(peerId))
            result.push_back({peer->name,peer->intake(""),peer->role});
    }
    return result;
}

Json Bots::cardLocked(std::string_view id) const {
    return cardFromDirectory(id,path(id));
}

Json Bots::cardFromDirectory(std::string_view id, const std::filesystem::path& botDir) const {
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

    Json item = {{"id",id},{"name",name},{"soul",readText(botDir / "SOUL.md",maxBotRoleBytes,true)},{"cron",schedules}};
    try { item["modelCallLimit"] = modelCallLimit(config); } catch (const Conflict&) {}
    for (const std::string_view key : {"environmentId","projectId","model"}) {
        if (key == "model") {
            const auto provider = config.find("modelProvider");
            if (provider == config.end() || *provider != "chatgpt") continue;
        }
        if (const auto value = config.find(key); value != config.end() && value->is_string() &&
            validPlainId(value->get_ref<const std::string&>())) item[key] = *value;
    }
    if (config.contains("role") && config["role"].is_string() && validBotRole(config["role"].get_ref<const std::string&>()))
        item["role"] = config["role"];
    if (const auto stored = config.find("avatar"); stored != config.end() && stored->is_object()) {
        auto avatar = *stored;
        if (avatar.value("kind","") == "upload") {
            const auto file = avatar.value("file",std::string{});
            const auto target = botDir / file;
            if (!AvatarImage::isStoredFilename(file) || !isAvatarFile(target)) {
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

Json Bots::skillCards() const {
    std::lock_guard lock(mutex_);
    auto items = Json::array();
    const auto botIds = ids();
    if (botIds.empty()) return items;
    // A cached skill may still carry this ID after the profile directory is removed.
    const auto issued = directory_ / ".flock-issued-ids";
    std::filesystem::create_directories(issued);
    for (const auto& id : botIds) {
        writeNewFile(issued / id,"");
        try {
            const auto card = cardLocked(id);
            items.push_back({{"id",id},{"name",card.at("name")},{"role",card.value("role",std::string("custom"))}});
        } catch (const Conflict&) {
            items.push_back({{"id",id},{"name",id},{"role","custom"}});
        }
    }
    return items;
}

bool Bots::reconcileCreation(const BotChange& change) {
    if (!validBotId(change.target) || !validBotId(change.requestKey))
        throw Conflict("Invalid Bot creation request");
    std::lock_guard lock(mutex_);
    if (std::filesystem::exists(directory_ / change.target)) {
        const auto config = Json::parse(readText(path(change.target) / "config.json",maxConfigBytes),nullptr,false);
        const auto request = config.find("creationRequest");
        return request != config.end() && request->is_string() && *request == change.requestKey;
    }
    const auto reservation = directory_ / ".flock-issued-ids" / change.target;
    if (std::filesystem::exists(reservation) && readText(reservation,128) == change.requestKey)
        std::filesystem::remove(reservation);
    return false;
}

void Bots::apply(const BotChange& change) try {
    if (change.operation == "rename_bot") {
        updateProfile(change.target,{{"name",change.name}});
        return;
    }
    if (change.operation != "create_bot" || !validBotId(change.target) || !validBotId(change.requestKey))
        throw Conflict("Invalid Bot creation request");
    const auto name = validatedBotName({{"name",change.name}});
    std::lock_guard lock(mutex_);
    std::filesystem::create_directories(directory_);
    const auto destination = directory_ / change.target;
    if (std::filesystem::exists(destination)) {
        try {
            const auto existing = Json::parse(readText(path(change.target) / "config.json",maxConfigBytes),nullptr,false);
            const auto request = existing.find("creationRequest");
            if (request != existing.end() && request->is_string() && *request == change.requestKey) return;
        } catch (const Conflict&) {
            // An unreadable profile cannot establish a replay.
        }
        throw Conflict("Target Bot already exists");
    }
    const auto issued = directory_ / ".flock-issued-ids";
    std::filesystem::create_directories(issued);
    const auto reservation = issued / change.target;
    if (std::filesystem::exists(reservation) && readText(reservation,128) != change.requestKey)
        throw Conflict("This Bot ID has already been used");
    const auto staging = directory_ / (".create-" + change.requestKey);
    if (!std::filesystem::create_directory(staging) &&
        (std::filesystem::is_symlink(staging) || !std::filesystem::is_directory(staging)))
        throw Conflict("Invalid Bot staging directory");
    const Json config{{"name",name},{"role","custom"},{"creationRequest",change.requestKey},
                      {"avatar",{{"kind","shape"},{"example","sprout"},{"color","#69c9ac"}}}};
    writeAtomic(staging / "config.json",config.dump(2) + "\n");
    if (!writeNewFile(reservation,change.requestKey) && readText(reservation,128) != change.requestKey)
        throw Conflict("This Bot ID has already been used");
    std::error_code failure;
    std::filesystem::rename(staging,destination,failure);
    if (failure) {
        std::error_code ignore;
        std::filesystem::remove(reservation,ignore);
        throw Conflict("Unable to publish Bot profile");
    }
} catch (const std::filesystem::filesystem_error&) {
    throw Conflict("Unable to save Bot profile");
}

Json Bots::create(const Json& profile, const std::optional<AvatarImage>& image,
                  const Policy::PublishOwnedBot& publishOwned) {
    if (!profile.is_object()) throw Conflict("Profile must be an object");
    const auto name = validatedBotName(profile);
    std::string role = "custom";
    if (profile.contains("role")) {
        if (!profile["role"].is_string() || !validBotRole(profile["role"].get_ref<const std::string&>()))
            throw Conflict("Unsupported bot role");
        role = profile["role"].get<std::string>();
    }
    const auto avatar = prepareAvatar(profile,{},Json{},image);

    std::lock_guard lock(mutex_);
    std::filesystem::create_directories(directory_);
    const auto issued = directory_ / ".flock-issued-ids";
    std::filesystem::create_directories(issued);
    const auto base = slugForName(name);
    const auto staging = directory_ / (".create-web-" + actionId());
    std::string id = base;
    bool reserved = false;
    bool published = false;
    try {
        for (unsigned suffix = 2; ; ++suffix) {
            if (!std::filesystem::exists(directory_ / id) && writeNewFile(issued / id,"")) break;
            id = base + "-" + std::to_string(suffix);
        }
        reserved = true;
        if (!std::filesystem::create_directory(staging)) throw Conflict("Unable to stage Bot profile");
        Json config = {{"name",name},{"role",role},{"avatar",avatar}};
        saveProfile(staging,config,image ? &*image : nullptr);
        const auto starter = roleStarterSoul(role);
        if (!starter.empty()) writeAtomic(staging / "SOUL.md",starter);
        auto card = cardFromDirectory(id,staging);
        const auto publish = [&] {
            std::filesystem::rename(staging,directory_ / id);
            published = true;
        };
        if (publishOwned) publishOwned(id,publish);
        else publish();
        return card;
    } catch (...) {
        std::error_code ignore;
        std::filesystem::remove_all(published ? directory_ / id : staging,ignore);
        if (reserved && !ignore) std::filesystem::remove(issued / id,ignore);
        throw;
    }
}

Json Bots::updateProfile(std::string_view id, const Json& profile, const std::optional<AvatarImage>& image) {
    if (!profile.is_object()) throw Conflict("Profile must be an object");
    if (botRunSettingCount(profile)) throw Conflict("Save how the Bot runs separately from its profile");

    std::lock_guard lock(mutex_);
    const auto botDir = path(id);
    const auto file = botDir / "config.json";
    auto config = readBotConfig(file);
    if (const auto soul = profile.find("soul"); soul != profile.end()) {
        const auto original = profile.find("originalSoul");
        if (profile.size() != 2 || original == profile.end() || !original->is_string() || image)
            throw Conflict("Save the role separately from other settings with its original text");
        const auto text = validatedBotSoul(*soul);
        const auto roleFile = botDir / "SOUL.md";
        const auto& expected = original->get_ref<const std::string&>();
        if (expected.size() > maxBotRoleBytes || expected.find('\0') != expected.npos ||
            readUpTo(roleFile,maxBotRoleBytes + 1,true) != expected)
            throw BotProfileError("role_changed","The Bot role changed or contains text the editor could not show");
        if (text.empty()) {
            std::error_code failure;
            std::filesystem::remove(roleFile,failure);
            if (failure) throw Conflict("Unable to clear the Bot role");
        } else {
            writeAtomic(roleFile,text);
        }
        return cardFromDirectory(id,botDir);
    }

    const auto existingAvatar = config.contains("avatar") ? config["avatar"] : Json{};
    const bool changeAvatar = profile.contains("avatar");
    config["name"] = validatedBotName(profile);
    if (changeAvatar)
        config["avatar"] = prepareAvatar(profile,botDir,existingAvatar,image);

    const auto previousFile = changeAvatar ? storedAvatarFile(existingAvatar) : std::string{};
    saveProfile(botDir,config,changeAvatar && image ? &*image : nullptr,previousFile);
    if (!previousFile.empty() && previousFile != storedAvatarFile(config.at("avatar")) && isAvatarFile(botDir / previousFile)) {
        std::error_code ignore;
        std::filesystem::remove(botDir / previousFile,ignore);
    }
    return cardLocked(id);
}

Json Bots::updateRunSettings(Store& store, std::string_view id, const Json& settings, Seconds now,
                             const ScheduleReporter& report) {
    if (!settings.is_object() || settings.empty() || settings.size() != botRunSettingCount(settings))
        throw BotProfileError("invalid_profile_update","Save how the Bot runs separately from its profile");
    std::vector<std::string> configErrors;
    const auto importErrors = store.syncSchedules(id,now,[&](const auto& commit) {
        std::lock_guard lock(mutex_);
        const auto file = path(id) / "config.json";
        const auto original = readText(file,maxConfigBytes);
        auto config = parseBotConfig(original);
        for (const auto& [key,value] : settings.items()) {
            auto setting = validatedBotRunSetting(key,value);
            if (setting == "") {
                config.erase(key);
                if (key == "model") config.erase("modelProvider");
            } else {
                config[key] = std::move(setting);
                if (key == "model") config["modelProvider"] = "chatgpt";
            }
        }
        if (config.contains("projectId") && !config.contains("environmentId"))
            throw BotProfileError("invalid_workspace","A pinned T3 project needs its environmentId");
        const auto updated = profileText(config);
        const auto schedules = configuredSchedules(id,config,configErrors);
        bool published = false;
        try {
            commit(schedules,[&] {
                writeAtomic(file,updated);
                published = true;
            });
        } catch (...) {
            if (published) writeAtomic(file,original);
            throw;
        }
    });
    reportScheduleErrors(id,importErrors,configErrors,report);
    std::lock_guard lock(mutex_);
    return cardLocked(id);
}

std::optional<AvatarImage> Bots::avatarImage(std::string_view id) const {
    std::lock_guard lock(mutex_);
    try {
        const auto botDir = path(id);
        const auto config = Json::parse(readText(botDir / "config.json",maxConfigBytes),nullptr,false);
        const auto avatar = config.find("avatar");
        if (avatar == config.end()) return std::nullopt;
        const auto file = storedAvatarFile(*avatar);
        if (file.empty()) return std::nullopt;
        const auto target = botDir / file;
        if (!isAvatarFile(target)) return std::nullopt;
        return AvatarImage(readText(target,AvatarImage::maxBytes));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void Bots::syncBotSchedules(Store& store, std::string_view id, Seconds now, const ScheduleReporter& report) const {
    std::vector<ConfiguredSchedule> schedules;
    std::vector<std::string> errors;
    try {
        schedules = configuredSchedules(id,readBotConfig(path(id) / "config.json"),errors);
    } catch (const Conflict& error) {
        errors.emplace_back(error.what());
    } catch (const Json::exception& error) {
        errors.emplace_back(error.what());
    }
    reportScheduleErrors(id,store.syncSchedules(id,schedules,now),errors,report);
}

void Bots::syncSchedules(Store& store, Seconds now, const ScheduleReporter& report) const {
    const auto botIds = ids();
    for (const auto& id : botIds) syncBotSchedules(store,id,now,report);
    store.pruneAutomations(botIds);
}
}
