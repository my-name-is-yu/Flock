#include "t3_setup.hpp"

#include "flock/skill_export.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sys/file.h>
#include <unistd.h>
#include <toml++/toml.hpp>

namespace flockv0::app {
namespace {

namespace fs = std::filesystem;

std::string setting(const char* name, std::string fallback = {}) {
    const auto value = std::getenv(name);
    return value && *value ? value : std::move(fallback);
}

fs::path userPath(const char* overrideName, std::string_view relative) {
    if (const auto path = setting(overrideName); !path.empty()) return path;
    const auto directory = setting("HOME");
    if (directory.empty()) throw Conflict("Cannot locate provider configuration without a user directory");
    return fs::path(directory) / relative;
}

bool installed(const std::string& command) {
    if (command.find('/') != command.npos) return access(command.c_str(),X_OK) == 0;
    auto paths = setting("PATH");
    std::size_t start = 0;
    while (start < paths.size()) {
        const auto end = paths.find(':',start);
        const auto directory = paths.substr(start,end-start);
        if (!directory.empty() && access((fs::path(directory) / command).c_str(),X_OK) == 0) return true;
        if (end == paths.npos) break;
        start = end+1;
    }
    return false;
}

void saveConfig(const fs::path& path, std::string_view content) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    auto temporary = path.string() + ".flock.XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) throw Conflict("Could not create a private Codex configuration file");
    FILE* output = fdopen(fd,"wb");
    if (!output) {
        close(fd);
        fs::remove(temporary);
        throw Conflict("Could not open the Codex configuration for writing");
    }
    const bool written = std::fwrite(content.data(),1,content.size(),output) == content.size();
    const auto closed = std::fclose(output);
    std::error_code error;
    if (written && closed == 0) fs::rename(temporary,path,error);
    if (!written || closed != 0 || error) {
        fs::remove(temporary);
        throw Conflict("Could not save the Codex configuration");
    }
}

using OwnsBearer = std::function<bool(std::string_view)>;

bool matchesBearer(std::string_view header, const OwnsBearer& owns) {
    constexpr std::string_view prefix = "Bearer ";
    return header.starts_with(prefix) && owns(header.substr(prefix.size()));
}

struct ConfigEdit {
    fs::path path;
    std::optional<std::string> original;
    std::string content;

    explicit ConfigEdit(fs::path file) : path(std::move(file)) {
        if (fs::is_symlink(path)) throw Conflict("Provider configuration cannot be a symlink");
        std::ifstream stream(path,std::ios::binary);
        if (!stream && fs::exists(path)) throw Conflict("Could not read provider configuration");
        if (stream) original.emplace(std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>());
        content = original.value_or("");
    }

    bool changed() const { return content != original.value_or(""); }
    void apply() const { if (changed()) saveConfig(path,content); }
    void restore() const {
        if (!changed()) return;
        if (original) saveConfig(path,*original);
        else fs::remove(path);
    }
};

ConfigEdit editCodex(std::string_view url, std::string_view token, const OwnsBearer& owns) {
    constexpr std::string_view begin = "# BEGIN Flock T3 connection\n";
    constexpr std::string_view end = "# END Flock T3 connection\n";
    ConfigEdit edit(userPath("FLOCK_CODEX_CONFIG",".codex/config.toml"));
    auto& config = edit.content;
    const auto start = config.find(begin);
    if (start != config.npos) {
        const auto finish = config.find(end,start + begin.size());
        if (finish == config.npos) throw Conflict("Incomplete Flock entry in Codex configuration");
        if (url.empty()) {
            const auto parsed = toml::parse(config);
            const auto header = parsed["mcp_servers"]["flock"]["http_headers"]["Authorization"]
                .value_or(std::string{});
            if (!matchesBearer(header,owns)) return edit;
        }
        config.erase(start,finish + end.size() - start);
    }
    if (!url.empty()) {
        try {
            const auto parsed = toml::parse(config);
            if (parsed["mcp_servers"]["flock"])
                throw Conflict("Remove the existing Codex MCP server named flock before connecting");
        } catch (const toml::parse_error&) {
            throw Conflict("Fix the invalid Codex configuration before connecting");
        }
        if (!config.empty() && config.back() != '\n') config += '\n';
        config += begin;
        config += "[mcp_servers.flock]\nurl = " + Json(url).dump() +
                  "\nhttp_headers = { Authorization = " + Json("Bearer " + std::string(token)).dump() + " }\n";
        config += end;
        try {
            (void)toml::parse(config);
        } catch (const toml::parse_error&) {
            throw Conflict("The Codex configuration cannot accept the Flock MCP server");
        }
    }
    return edit;
}

ConfigEdit editClaude(std::string_view url, std::string_view token, const OwnsBearer& owns) {
    const auto directory = setting("CLAUDE_CONFIG_DIR");
    ConfigEdit edit(directory.empty() ? userPath("FLOCK_CLAUDE_CONFIG",".claude.json")
                                     : fs::path(directory) / ".claude.json");
    if (url.empty() && !edit.original) return edit;
    auto config = edit.original ? Json::parse(*edit.original) : Json::object();
    if (!config.is_object()) throw Conflict("Invalid Claude Code configuration");
    if (url.empty() && !config.contains("mcpServers")) return edit;
    auto& servers = config["mcpServers"];
    if (servers.is_null()) servers = Json::object();
    if (!servers.is_object()) throw Conflict("Invalid Claude Code MCP configuration");
    if (const auto current = servers.find("flock"); current != servers.end()) {
        const auto headers = current->is_object() ? current->value("headers",Json::object()) : Json::object();
        const auto header = headers.is_object() ? headers.value("Authorization",std::string{}) : std::string{};
        if (!matchesBearer(header,owns)) {
            if (url.empty()) return edit;
            throw Conflict("Remove the existing Claude Code MCP server named flock before connecting");
        }
    } else if (url.empty()) return edit;
    if (url.empty()) servers.erase("flock");
    else servers["flock"] = {{"type","http"},{"url",url},{"headers",{{"Authorization","Bearer " + std::string(token)}}}};
    edit.content = config.dump(2) + '\n';
    return edit;
}

}

T3SetupLock::T3SetupLock() {
    const auto directory = userPath("HOME","") / ".codex";
    fs::create_directories(directory);
    fd_ = open((directory / ".t3-setup.lock").c_str(),O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,0600);
    if (fd_ < 0) throw Conflict("Could not open the T3 setup lock");
    if (flock(fd_,LOCK_EX | LOCK_NB) != 0) {
        close(fd_);
        throw Conflict("Another T3 setup command is running");
    }
}

T3SetupLock::~T3SetupLock() { close(fd_); }

Json configureT3Providers(std::string_view url,
    const std::function<void(const InstallT3Provider&)>& connect) {
    auto providers = Json::array();
    std::vector<ConfigEdit> edits;
    std::size_t applied = 0;
    try {
        connect([&](std::string_view token) {
            if (installed(setting("FLOCK_CODEX_CLI","codex"))) {
                edits.push_back(editCodex(url,token,{}));
                providers.push_back("codex");
            }
            if (installed(setting("FLOCK_CLAUDE_CLI","claude"))) {
                edits.push_back(editClaude(url,token,[](std::string_view bearer) {
                    return bearer.starts_with("flock-t3-");
                }));
                providers.push_back("claude");
            }
            for (const auto& edit : edits) {
                edit.apply();
                ++applied;
            }
        });
    } catch (...) {
        bool rollbackFailed = false;
        while (applied) {
            try { edits[--applied].restore(); }
            catch (...) { rollbackFailed = true; }
        }
        if (rollbackFailed) throw Conflict("Could not restore provider configuration after T3 setup failed");
        throw;
    }
    return providers;
}

bool t3ProvidersNeedRefresh(const OwnsBearer& owns) {
    if (installed(setting("FLOCK_CODEX_CLI","codex")) && !editCodex({},{},owns).changed()) return true;
    return installed(setting("FLOCK_CLAUDE_CLI","claude")) && !editClaude({},{},owns).changed();
}

void removeT3Providers(const OwnsBearer& owns) {
    const auto codex = editCodex({},{},owns);
    const auto claude = editClaude({},{},owns);
    codex.apply();
    try { claude.apply(); }
    catch (...) {
        codex.restore();
        throw;
    }
}

void syncT3Skills(const Json& bots) {
    const auto claude = setting("FLOCK_CLAUDE_CLI","claude");
    const auto codex = setting("FLOCK_CODEX_CLI","codex");
    std::string errors;
    const auto sync = [&](const std::string& cli, const char* overrideName, std::string_view path) {
        if (!installed(cli)) return;
        try {
            exportFlockSkills(userPath(overrideName,path),bots);
        } catch (const std::exception& error) {
            if (!errors.empty()) errors += "\n";
            errors += error.what();
        }
    };
    sync(claude,"FLOCK_CLAUDE_SKILLS_DIR",".claude/skills");
    sync(codex,"FLOCK_CODEX_SKILLS_DIR",".agents/skills");
    if (!errors.empty()) throw Conflict(errors);
}

}
