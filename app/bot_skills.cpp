#include "bot_skills.hpp"
#include "bots.hpp"
#include "settings_file.hpp"
#include "t3_setup.hpp"
#include "flock/bot_id.hpp"
#include "flock/digest.hpp"
#include "flock/file_descriptor.hpp"
#include "flock/policy.hpp"
#include "flock/t3_connection.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/file.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string_view>

namespace flockv0::app {
namespace {
namespace fs = std::filesystem;
constexpr std::string_view marker = ".flock-skill";
constexpr std::string_view markerText = "Flock T3 skill\n";
constexpr std::size_t suffixLength = 12;

std::string skillName(std::string_view id, std::string_view name) {
    std::string slug;
    for (const unsigned char ch : name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
            slug += static_cast<char>(ch);
        else if (ch >= 'A' && ch <= 'Z')
            slug += static_cast<char>(ch - 'A' + 'a');
        else if (!slug.empty() && slug.back() != '-')
            slug += '-';
        if (slug.size() == 45) break;
    }
    if (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "bot";

    const bool reserved = slug.size() > suffixLength && slug[slug.size() - suffixLength - 1] == '-' &&
                          slug.find_last_not_of("0123456789abcdef") == slug.size() - suffixLength - 1;
    if (slug != id || reserved) slug += '-' + sha256Hex(id).substr(0, suffixLength);
    return "flock-" + slug;
}

std::string skillText(std::string_view skill, std::string_view name, std::string_view id) {
    return "---\nname: " + std::string(skill) + "\ndescription: " + Json("Ask " + std::string(name) + " in Flock.").dump() +
           "\n---\n\nUse the text after the skill invocation as the request for this Bot. Ask for the request if it is missing.\n"
           "Get the current thread ID from `t3_thread_list`. If there is no current thread, ask the person to open a T3 Session.\n"
           "Call `flock_ask` with `bot` set to `" +
           std::string(id) +
           "`, the request as `text`, the current `threadId`, and a fresh `requestId`. "
           "For a retry, reuse the same request ID and arguments. If Flock reports that the Bot is unavailable, tell the person.\n"
           "The Bot's reply arrives in the same Session.\n";
}

bool hasContents(const fs::path& path, std::string_view expected) {
    if (fs::symlink_status(path).type() != fs::file_type::regular || fs::file_size(path) != expected.size()) return false;
    std::ifstream file(path, std::ios::binary);
    return file && std::string(std::istreambuf_iterator<char>(file), {}) == expected;
}

bool managed(const fs::path& path) {
    return fs::symlink_status(path).type() == fs::file_type::directory && hasContents(path / marker, markerText);
}
}

void exportBotSkills(const fs::path& root, const Json& bots) {
    std::map<std::string, std::string> files;
    for (const auto& bot : bots) {
        const auto id = field(bot, "id");
        if (!validBotId(id)) continue;
        const auto name = field(bot, "name");
        const auto skill = skillName(id, name);
        if (!files.emplace(skill, skillText(skill, name, id)).second) throw Conflict("Duplicate Flock Bot skill name");
    }

    if (files.empty()) {
        if (!fs::is_directory(root)) return;
    } else {
        fs::create_directories(root);
    }
    fs::path blocked;
    for (const auto& [name, contents] : files) {
        const auto directory = root / name;
        if (fs::create_directory(directory)) {
            try {
                writePrivateFile(directory / marker, markerText);
            } catch (...) {
                std::error_code ignored;
                fs::remove(directory, ignored);
                throw;
            }
        } else if (!managed(directory)) {
            blocked = directory;
            continue;
        }
        const auto file = directory / "SKILL.md";
        if (fs::is_symlink(file)) {
            blocked = file;
            continue;
        }
        if (!hasContents(file, contents)) writePrivateFile(file, contents);
    }
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        if ((name != "flock" && !name.starts_with("flock-")) || files.contains(name) || !managed(entry.path())) continue;
        fs::remove(entry.path() / "SKILL.md");
        fs::remove(entry.path() / marker);
        std::error_code ignored;
        fs::remove(entry.path(), ignored);
    }
    if (!blocked.empty()) throw Conflict("Flock cannot update a user-managed skill at " + blocked.string());
}

void syncT3BotSkills(const fs::path& home, Store& store, const Bots& bots, const WebIdentityResolver& resolve) noexcept try {
    const T3Connections connections(store);
    if (!connections.configured()) return;
    const FileDescriptor lock(::open((home / ".t3-skills.lock").c_str(), O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_RDWR, 0600));
    if (lock.get() < 0 || ::flock(lock.get(), LOCK_EX) != 0) throw Conflict("Could not lock the Flock skill export");
    if (!connections.configured()) return;

    const Policy policy(store);
    auto cards = Json::array();
    const auto root = resolve(localOwner.id);
    for (const auto& card : root ? bots.cards() : Json::array()) {
        const auto id = field(card, "id");
        if (!policy.allows(Person{localOwner.id, *root}, Action::bot_decide, id)) continue;
        cards.push_back(policy.redactSecrets(id, {{"id", id}, {"name", field(card, "name")}}));
    }
    syncT3ProviderSkills([&](std::string_view token) { return connections.ownsBearer(token); }, cards);
} catch (const std::exception& error) {
    std::cerr << "Could not synchronize Flock Bot skills: " << error.what() << '\n';
}
}
