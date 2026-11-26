#include "flock/skill_export.hpp"
#include "flock/bot_id.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {

constexpr std::string_view marker = ".flock-skill";
constexpr std::string_view askInstructions =
    R"(Get threadId from currentThreadId in t3_thread_list. If it is missing, ask the person to run this skill in a T3 Session.
Call flock_ask with bot, threadId, the request text, and a fresh requestId. Retry the same request with identical arguments.
Bot messages arrive in this Session prefixed with the Bot's name. No polling is needed.
Show messages addressed to the person and let them answer. Forward their reply with flock_ask using the same bot and threadId and a new requestId.
Use flock_stop with bot and threadId when the person asks to remove the Bot from the Session. A new flock_ask brings it back.
)";

std::string slug(std::string_view id) {
    std::string name = "flock-";
    for (auto c : id) {
        if (c == '_' || c == '.') c = '-';
        if (c == '-' && name.back() == '-') continue;
        name += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        if (name.size() == 64) break;
    }
    if (name.back() == '-') name.pop_back();
    return name;
}

std::string shortHash(std::string_view id) {
    std::array<unsigned char, 32> bytes{};
    unsigned int size = 0;
    if (EVP_Digest(id.data(), id.size(), bytes.data(), &size, EVP_sha256(), nullptr) != 1 || size != bytes.size())
        throw std::runtime_error("Could not hash Bot ID");
    std::string result;
    for (unsigned i = 0; i < 6; ++i) {
        constexpr std::string_view digits = "0123456789abcdef";
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    return result;
}

std::string skill(std::string_view name, std::string_view description, std::string_view body) {
    // A JSON-quoted string is also a valid YAML double-quoted scalar.
    return "---\nname: " + std::string(name) + "\ndescription: " + Json(description).dump() + "\n---\n\n" + std::string(body);
}

void write(const std::filesystem::path& file, std::string_view text) {
    if (std::filesystem::is_symlink(file)) throw Conflict("Cannot update a symlink in a Flock skill");
    std::ifstream existing(file, std::ios::binary);
    if (existing && std::string(std::istreambuf_iterator<char>(existing), {}) == text) return;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << text;
    out.close();
}

bool owned(const std::filesystem::path& path) {
    if (std::filesystem::is_symlink(path) || !std::filesystem::is_directory(path) || std::filesystem::is_symlink(path / marker) ||
        !std::filesystem::is_regular_file(path / marker))
        return false;
    std::ifstream input(path / marker);
    std::string contents;
    return std::getline(input, contents) && contents == "Flock T3 skill";
}

}

std::map<std::string, std::string> flockSkillNames(const Json& bots) {
    std::map<std::string, std::string> names;
    std::map<std::string, unsigned> counts;
    for (const auto& bot : bots) {
        const auto id = bot.value("id", std::string{});
        if (validBotId(id)) {
            auto name = slug(id);
            if (names.emplace(id, name).second) ++counts[name];
        }
    }
    for (auto& [id, name] : names) {
        if (counts[name] < 2) continue;
        const auto base = name;
        unsigned attempt = 0;
        do {
            const auto hash = shortHash(attempt == 0 ? id : id + ":" + std::to_string(attempt));
            name = base.substr(0, 64 - hash.size() - 1);
            if (name.back() == '-') name.pop_back();
            name += "-" + hash;
            ++attempt;
        } while (counts.contains(name));
        counts.emplace(name, 1);
    }
    return names;
}

void exportFlockSkills(const std::filesystem::path& root, const Json& bots) {
    const auto names = flockSkillNames(bots);
    std::map<std::string, std::string> files{
        {"flock",
         skill("flock", "Ask a Flock Bot in the current T3 Session.",
               "Use flock_bots to find the intended Bot by name or role. Pass its exact id as bot.\n" + std::string(askInstructions))}};
    for (const auto& bot : bots) {
        const auto id = bot.value("id", std::string{});
        const auto found = names.find(id);
        if (found == names.end()) continue;
        const auto name = bot.value("name", id);
        const auto role = bot.value("role", std::string("custom"));
        files.emplace(found->second, skill(found->second, "Ask " + name + " (" + role + ") in Flock.",
                                           "Pass `" + id + "` as bot.\n" + std::string(askInstructions)));
    }
    std::filesystem::create_directories(root);
    std::vector<std::string> blocked;
    for (const auto& [name, contents] : files) {
        const auto directory = root / name;
        if (std::filesystem::create_directory(directory))
            write(directory / marker, "Flock T3 skill\n");
        else if (!owned(directory)) {
            blocked.push_back(name);
            continue;
        }
        write(directory / "SKILL.md", contents);
    }
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        if ((name == "flock" || name.starts_with("flock-")) && !files.contains(name) && owned(entry.path())) {
            std::filesystem::remove(entry.path() / "SKILL.md");
            // Leave any files the person added to this directory.
            std::filesystem::remove(entry.path() / marker);
            std::error_code error;
            std::filesystem::remove(entry.path(), error);
        }
    }
    if (!blocked.empty()) throw Conflict("Skill directory already exists: " + (root / blocked.front()).string());
}

}
