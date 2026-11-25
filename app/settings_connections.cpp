#include "settings_connections.hpp"
#include "bot_tools.hpp"
#include "git_github.hpp"
#include "settings_file.hpp"
#include "flock/channel_bridge_settings.hpp"
#include "flock/channels.hpp"

#include <algorithm>
#include <cstdlib>
#include <curl/curl.h>

namespace flockv0::app {
namespace {
namespace fs = std::filesystem;

std::string settingText(const Json& input, const char* key, std::size_t max, bool optional = false) {
    const auto value = input.at(key).get<std::string>();
    if ((!optional && value.find_first_not_of(" \t") == value.npos) || value.size() > max ||
        std::ranges::any_of(value, [](unsigned char c) { return c < 32 || c == 127; }))
        throw Conflict("Invalid " + std::string(key));
    return value;
}

std::string credential(const Json& input, const char* key) {
    if (input.at(key).is_null()) return {};
    auto value = settingText(input, key, 4096);
    if (!std::ranges::all_of(value, [](unsigned char c) { return c >= 33 && c <= 126; }))
        throw Conflict("Credentials cannot contain whitespace");
    return value;
}

std::string environmentValue(const char* name) {
    const auto value = std::getenv(name);
    return value ? value : "";
}

std::string channelType(const Json& input) {
    const auto type = input.at("type").get<std::string>();
    if (type != "slack" && type != "imessage") throw Conflict("Choose Slack or iMessage");
    return type;
}

const Json& objectField(const Json& input, std::string_view key) {
    static const auto empty = Json::object();
    const auto it = input.find(key);
    return it == input.end() ? empty : *it;
}

std::string githubCredential(const Json& config, const char* key) {
    return config.value(key, "");
}

bool savedEntryChanged(const Json& active, const std::string& id, const Json& saved) {
    const auto found = active.find(id);
    return found == active.end() || *found != saved;
}

std::string publicEndpoint(std::string url) {
    if (const auto suffix = url.find_first_of("?#"); suffix != url.npos) url.resize(suffix);
    return url;
}

void changeGithub(Json& settings, const Json& input) {
    if (input.contains("token")) {
        auto token = credential(input, "token");
        if (!token.empty()) (void)makeGitHubHost(token);
        settings["token"] = std::move(token);
    }
    if (input.contains("webhookSecret")) settings["webhookSecret"] = credential(input, "webhookSecret");
}

void changeChannels(Json& settings, const Json& input) {
    const auto action = input.at("action").get<std::string>();
    if (action == "options") {
        if (input.contains("artifactBaseUrl"))
            settings["artifactBaseUrl"] = normalizeArtifactBaseUrl(settingText(input, "artifactBaseUrl", 2048, true));
        if (input.contains("sessionCredential")) settings["sessionCredential"] = credential(input, "sessionCredential");
        return;
    }
    const auto id = settingText(input, "id", 128);
    if (action == "saveMachine" || action == "removeMachine") {
        auto& machines = settings["localMachines"];
        if (machines.is_null()) machines = Json::object();
        if (action == "saveMachine")
            machines[id] = settingText(input, "personId", 200);
        else
            machines.erase(id);
        return;
    }
    if (action != "saveBridge" && action != "removeBridge") throw Conflict("Unknown channel operation");
    auto& bridges = settings[channelType(input)];
    if (bridges.is_null()) bridges = Json::object();
    if (action == "removeBridge") {
        bridges.erase(id);
        return;
    }
    auto& entry = bridges[id];
    if (entry.is_null()) entry = Json{{"senders", Json::object()}};
    const auto& port = input.at("port");
    if (!port.is_number_integer() || port.get<std::uint64_t>() == 0 || port.get<std::uint64_t>() > 65535)
        throw Conflict("Enter a bridge port from 1 to 65535");
    if (entry.contains("bridgePort") && entry.at("bridgePort") != port && !input.contains("credential"))
        throw Conflict("Enter the bridge credential again when changing its port");
    entry["bridgePort"] = port;
    if (input.contains("credential")) entry["credential"] = credential(input, "credential");
    if (!entry.contains("credential")) throw Conflict("Enter the bridge credential");
}

fs::path managedSkill(const fs::path& home, const std::string& directory) {
    auto relative = fs::path(directory);
    if (relative.is_absolute()) throw Conflict("Enter a folder inside the Flock skills folder");
    if (relative.begin() != relative.end() && *relative.begin() == "skills") relative = relative.lexically_relative("skills");
    std::error_code error;
    const auto state = fs::canonical(home, error);
    if (error) throw Conflict("The Flock data folder is unavailable");
    const auto root = state / "skills";
    const auto folder = fs::canonical(root / relative, error);
    if (error || folder == root) throw Conflict("Select an existing folder inside the Flock skills folder");
    const auto inside = folder.lexically_relative(root);
    if (inside.empty() || *inside.begin() == ".." || fs::canonical(root, error) != root || error)
        throw Conflict("Enter a folder inside the Flock skills folder");
    const auto document = folder / "SKILL.md";
    if (fs::symlink_status(document, error).type() != fs::file_type::regular || error) throw Conflict("This folder needs a SKILL.md file");
    return fs::path("skills") / inside;
}

bool skillAvailable(const fs::path& home, const std::string& directory) {
    try {
        return managedSkill(home, directory) == fs::path(directory);
    } catch (const Conflict&) {
        return false;
    }
}

Json activeToolSettings(const fs::path& home, Json settings) {
    auto& skills = settings["skills"];
    if (skills.is_null()) skills = Json::object();
    for (const auto& [id, path] : objectField(settings, "managedSkills").items()) {
        const auto directory = path.get<std::string>();
        if (skillAvailable(home, directory)) skills[id] = directory;
    }
    return settings;
}

void changeTools(Json& settings, const fs::path& home, const Json& input) {
    const auto action = input.at("action").get<std::string>();
    if (action == "search") {
        settings["searchKey"] = credential(input, "key");
        return;
    }
    const auto id = settingText(input, "id", 128);
    if (!validBotId(id)) throw Conflict("Use a lowercase ID with letters, digits or hyphens");
    if (action == "saveMcp" || action == "removeMcp") {
        auto& servers = settings["mcpServers"];
        if (servers.is_null()) servers = Json::object();
        if (action == "removeMcp")
            servers.erase(id);
        else {
            auto url = settingText(input, "url", 2048);
            if (url.find_first_of("?#") != url.npos) throw Conflict("Enter the server URL without query parameters or a fragment");
            auto& entry = servers[id];
            // A token stays bound to its original endpoint.
            if (!entry.is_object() || entry.value("url", "") != url) entry = Json::object();
            entry["url"] = std::move(url);
            if (input.contains("token")) entry["token"] = credential(input, "token");
        }
        return;
    }
    if (action != "saveSkill" && action != "removeSkill") throw Conflict("Unknown tool operation");
    if (id == "catalog") throw Conflict("Choose a different skill ID");
    if (settings.contains("skills")) settings.at("skills").erase(id);
    auto& managed = settings["managedSkills"];
    if (managed.is_null()) managed = Json::object();
    if (action == "removeSkill")
        managed.erase(id);
    else
        managed[id] = managedSkill(home, settingText(input, "path", 1024)).string();
    if (managed.size() + objectField(settings, "skills").size() > 128) throw Conflict("Flock supports up to 128 shared skills");
}

bool bridgeReachable(unsigned port) {
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) return false;
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return false;
    const auto address = "http://127.0.0.1:" + std::to_string(port);
    curl_easy_setopt(curl.get(), CURLOPT_URL, address.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http");
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 1500L);
    return curl_easy_perform(curl.get()) == CURLE_OK;
}

void appendSender(Json& senders, const std::string& id, const Json& sender) {
    senders.push_back({{"id", id}, {"personId", sender.at("personId")}, {"botId", sender.at("botId")}});
}
}

SettingsConnections::SettingsConnections(const fs::path& home)
    : home_(home), file_(home / "settings-connections.json"), persisted_(readSettingsFile(file_)), desired_(persisted_) {
    if (!persisted_.is_object()) throw Conflict("Invalid saved connection settings");
    auto github =
        Json{{"token", environmentValue("FLOCK_GITHUB_TOKEN")}, {"webhookSecret", environmentValue("FLOCK_GITHUB_WEBHOOK_SECRET")}};
    github.update(objectField(persisted_, "github"));
    desired_["github"] = std::move(github);
    const auto channelFile = environmentValue("FLOCK_CHANNEL_BRIDGES_FILE");
    hostChannels_ = !channelFile.empty();
    if (hostChannels_)
        desired_["channels"] = readChannelBridgeSettings(channelFile);
    else if (!desired_.contains("channels"))
        desired_["channels"] = Json::object();
    hostTools_ = !environmentValue("FLOCK_WEB_SEARCH_KEY").empty() || fs::exists(home_ / "bot-tools.json");
    if (hostTools_ || !desired_.contains("tools")) desired_["tools"] = readBotToolSettings(home_);
    active_ = desired_;
    loadedTools_ = activeToolSettings(home_, active_.at("tools"));
}

std::string SettingsConnections::githubToken() const {
    return githubCredential(active_.at("github"), "token");
}
std::string SettingsConnections::githubWebhookSecret() const {
    return githubCredential(active_.at("github"), "webhookSecret");
}

void SettingsConnections::configureChannels(Store& store, WebCallbacks& callbacks) const {
    configureManagedChannelBridges(store, active_.at("channels"), callbacks);
}

std::shared_ptr<const BotTools> SettingsConnections::loadTools() const {
    return loadBotTools(home_, loadedTools_);
}

Json SettingsConnections::viewLocked() const {
    const auto& github = desired_.at("github");
    const auto& channels = desired_.at("channels");
    const auto& tools = desired_.at("tools");
    const auto& activeChannels = active_.at("channels");
    const auto& activeTools = active_.at("tools");
    auto bridges = Json::array();
    auto activeBridgeCount = 0u;
    for (const auto* type : {"slack", "imessage"}) {
        const auto& activeBridges = objectField(activeChannels, type);
        activeBridgeCount += activeBridges.size();
        for (const auto& [id, bridge] : objectField(channels, type).items()) {
            auto senders = Json::array();
            for (const auto& [senderId, sender] : bridge.at("senders").items()) appendSender(senders, senderId, sender);
            const bool pending = savedEntryChanged(activeBridges, id, bridge);
            const auto checked = bridgeChecks_.find({type, id});
            bridges.push_back({{"type", type},
                               {"id", id},
                               {"port", bridge.at("bridgePort")},
                               {"senders", std::move(senders)},
                               {"credentialSet", !bridge.value("credential", "").empty()},
                               {"active", !pending},
                               {"pending", pending},
                               {"reachable", checked == bridgeChecks_.end() ? Json(nullptr) : Json(checked->second.passed)},
                               {"checkedAt", checked == bridgeChecks_.end() ? Json(nullptr) : Json(checked->second.finishedAt)}});
        }
    }
    auto machines = Json::array();
    const auto& activeMachines = objectField(activeChannels, "localMachines");
    for (const auto& [id, person] : objectField(channels, "localMachines").items())
        machines.push_back({{"id", id}, {"personId", person}, {"pending", savedEntryChanged(activeMachines, id, person)}});
    auto servers = Json::array();
    const auto& activeServers = objectField(activeTools, "mcpServers");
    for (const auto& [id, server] : objectField(tools, "mcpServers").items())
        servers.push_back({{"id", id},
                           {"url", publicEndpoint(server.at("url").get<std::string>())},
                           {"tokenSet", !server.value("token", "").empty()},
                           {"pending", savedEntryChanged(activeServers, id, server)}});
    auto skills = Json::array();
    const auto& loadedSkills = objectField(loadedTools_, "skills");
    const auto& activeLegacySkills = objectField(activeTools, "skills");
    const auto& activeManagedSkills = objectField(activeTools, "managedSkills");
    const bool searchPending = tools.value("searchKey", "") != activeTools.value("searchKey", "");
    const bool mcpPending = objectField(tools, "mcpServers") != activeServers;
    bool skillsPending = objectField(tools, "skills") != activeLegacySkills || objectField(tools, "managedSkills") != activeManagedSkills;
    for (const auto& [id, path] : objectField(tools, "skills").items())
        skills.push_back({{"id", id}, {"path", path}, {"available", true}, {"pending", savedEntryChanged(activeLegacySkills, id, path)}});
    for (const auto& [id, path] : objectField(tools, "managedSkills").items()) {
        const bool available = skillAvailable(home_, path.get<std::string>());
        const bool active = available && !savedEntryChanged(loadedSkills, id, path);
        const bool pending = savedEntryChanged(activeManagedSkills, id, path) || (available && !active);
        skillsPending = skillsPending || pending;
        skills.push_back({{"id", id}, {"path", path}, {"available", available}, {"active", active}, {"pending", pending}});
    }
    return {{"github",
             {{"tokenSet", !githubCredential(github, "token").empty()},
              {"webhookSecretSet", !githubCredential(github, "webhookSecret").empty()},
              {"activeTokenSet", !githubToken().empty()},
              {"activeWebhookSecretSet", !githubWebhookSecret().empty()},
              {"verified", githubCheck_ ? Json(githubCheck_->passed) : Json(nullptr)},
              {"checkedAt", githubCheck_ ? Json(githubCheck_->finishedAt) : Json(nullptr)},
              {"pending", github != active_.at("github")}}},
            {"channels",
             {{"bridges", std::move(bridges)},
              {"localMachines", std::move(machines)},
              {"artifactBaseUrl", channels.value("artifactBaseUrl", "")},
              {"sessionCredentialSet", !channels.value("sessionCredential", "").empty()},
              {"slackPending", objectField(channels, "slack") != objectField(activeChannels, "slack")},
              {"imessagePending", objectField(channels, "imessage") != objectField(activeChannels, "imessage")},
              {"localMachinesPending", objectField(channels, "localMachines") != activeMachines},
              {"optionsPending", channels.value("artifactBaseUrl", "") != activeChannels.value("artifactBaseUrl", "") ||
                                     channels.value("sessionCredential", "") != activeChannels.value("sessionCredential", "")},
              {"activeBridgeCount", activeBridgeCount},
              {"pending", channels != activeChannels},
              {"hostManaged", hostChannels_}}},
            {"tools",
             {{"searchKeySet", !tools.value("searchKey", "").empty()},
              {"mcpServers", std::move(servers)},
              {"skills", std::move(skills)},
              {"searchPending", searchPending},
              {"mcpPending", mcpPending},
              {"skillsPending", skillsPending},
              {"pending", searchPending || mcpPending || skillsPending},
              {"hostManaged", hostTools_}}}};
}

Json SettingsConnections::view() const {
    std::lock_guard lock(mutex_);
    return viewLocked();
}

void SettingsConnections::saveLocked(std::string_view section, Json desired, Json savedSection) {
    if (savedSection.is_null()) savedSection = desired;
    if (desired_.at(section) == desired && objectField(persisted_, section) == savedSection) return;
    auto saved = persisted_;
    saved[std::string(section)] = std::move(savedSection);
    writeSettingsFile(file_, saved);
    if (section == "github" && githubCredential(desired_.at(section), "token") != githubCredential(desired, "token")) githubCheck_.reset();
    if (section == "channels") {
        std::erase_if(bridgeChecks_, [&](const auto& item) {
            const auto& [type, id] = item.first;
            const auto& previous = objectField(desired_.at(section), type);
            const auto& updated = objectField(desired, type);
            return !previous.contains(id) || !updated.contains(id) || previous.at(id).at("bridgePort") != updated.at(id).at("bridgePort");
        });
    }
    persisted_ = std::move(saved);
    desired_[std::string(section)] = std::move(desired);
}

Json SettingsConnections::change(std::string_view section, const Json& input) {
    if (!input.is_object()) throw Conflict("Invalid connection settings");
    try {
        const auto action = input.value("action", "");
        if (section == "github" && action == "check") {
            std::string token;
            {
                std::lock_guard lock(mutex_);
                token = githubCredential(desired_.at("github"), "token");
            }
            if (token.empty()) throw Conflict("Save a GitHub token first");
            bool reachable = true;
            try {
                verifyGitHubToken(token);
            } catch (const std::exception&) {
                reachable = false;
            }
            std::lock_guard lock(mutex_);
            if (token != githubCredential(desired_.at("github"), "token")) throw Conflict("The GitHub token changed during the check");
            githubCheck_ = Check{reachable, unixTime()};
            auto result = viewLocked();
            result["check"] = {{"reachable", reachable},
                               {"checkedAt", githubCheck_->finishedAt},
                               {"message", reachable ? "GitHub accepted the token." : "GitHub could not verify the saved token."}};
            return result;
        }
        if (section == "channels" && action == "checkBridge") {
            const auto type = channelType(input);
            const auto id = settingText(input, "id", 128);
            unsigned port;
            {
                std::lock_guard lock(mutex_);
                const auto& bridges = objectField(desired_.at("channels"), type);
                if (!bridges.contains(id)) throw Conflict("This bridge is no longer registered");
                port = bridges.at(id).at("bridgePort").get<unsigned>();
                if (input.contains("port") && input.at("port") != port)
                    throw Conflict("The bridge address changed. Reopen this setting to check the saved port.");
            }
            const bool reachable = bridgeReachable(port);
            std::lock_guard lock(mutex_);
            const auto& bridges = objectField(desired_.at("channels"), type);
            if (!bridges.contains(id) || bridges.at(id).at("bridgePort") != port)
                throw Conflict("The bridge address changed during the check");
            const Check check{reachable, unixTime()};
            bridgeChecks_[{type, id}] = check;
            auto result = viewLocked();
            result["check"] = {{"reachable", reachable},
                               {"checkedAt", check.finishedAt},
                               {"message", reachable ? "The local bridge port is reachable." : "The local bridge port did not respond."}};
            return result;
        }
        std::lock_guard lock(mutex_);
        if (!desired_.contains(section)) throw Conflict("Unknown connection settings");
        auto desired = desired_.at(section);
        if (section == "github") {
            auto saved = objectField(persisted_, section);
            changeGithub(saved, input);
            desired.update(saved);
            saveLocked(section, std::move(desired), std::move(saved));
            return viewLocked();
        } else if (section == "channels") {
            if (hostChannels_) throw Conflict("These channel settings are managed on the Flock host");
            changeChannels(desired, input);
            validateChannelBridgeSettings(desired);
        } else if (section == "tools") {
            if (hostTools_) throw Conflict("These tool settings are managed on the Flock host");
            changeTools(desired, home_, input);
            (void)loadBotTools(home_, activeToolSettings(home_, desired));
        } else
            throw Conflict("Unknown connection settings");
        saveLocked(section, std::move(desired));
        return viewLocked();
    } catch (const Json::exception&) {
        throw Conflict("Invalid connection settings");
    }
}

Json SettingsConnections::botChannelsLocked(std::string_view botId) const {
    auto result = Json::array();
    const auto& channels = desired_.at("channels");
    for (const auto* type : {"slack", "imessage"}) {
        const auto& activeBridges = objectField(active_.at("channels"), type);
        for (const auto& [id, bridge] : objectField(channels, type).items()) {
            auto senders = Json::array();
            for (const auto& [senderId, sender] : bridge.at("senders").items())
                if (sender.at("botId") == botId) senders.push_back({{"id", senderId}, {"personId", sender.at("personId")}});
            const bool pending = !activeBridges.contains(id) || activeBridges.at(id) != bridge;
            result.push_back(
                {{"type", type}, {"id", id}, {"senders", std::move(senders)}, {"pending", pending}, {"hostManaged", hostChannels_}});
        }
    }
    return result;
}

Json SettingsConnections::botChannels(std::string_view botId) const {
    std::lock_guard lock(mutex_);
    return botChannelsLocked(botId);
}

Json SettingsConnections::changeBotChannel(std::string_view botId, const Json& input, const Store& store) {
    if (!validBotId(botId) || !input.is_object()) throw Conflict("Invalid Bot channel settings");
    if (hostChannels_) throw Conflict("These channel settings are managed on the Flock host");
    try {
        const auto action = input.at("action").get<std::string>();
        if (action != "setSender" && action != "removeSender") throw Conflict("Unknown Bot channel operation");
        const auto type = channelType(input);
        const auto id = settingText(input, "id", 128);
        const auto& sender = input.at("sender");
        const auto senderId = settingText(sender, "id", 200);
        std::lock_guard lock(mutex_);
        auto channels = desired_.at("channels");
        if (!objectField(channels, type).contains(id)) throw Conflict("This bridge is no longer registered");
        auto& senders = channels.at(type).at(id).at("senders");
        if (const auto existing = senders.find(senderId); existing != senders.end()) {
            const auto& assigned = existing->at("botId").get_ref<const std::string&>();
            if (assigned != botId && (action != "setSender" || !store.botDeleted(assigned)))
                throw Conflict("This sender is linked to another Bot");
        }
        if (action == "removeSender")
            senders.erase(senderId);
        else
            senders[senderId] = {{"personId", settingText(sender, "personId", 200)}, {"botId", botId}};
        validateChannelBridgeSettings(channels);
        saveLocked("channels", std::move(channels));
        return botChannelsLocked(botId);
    } catch (const Json::exception&) {
        throw Conflict("Invalid Bot channel settings");
    }
}
}
