#include "t3_setup.hpp"

#include "flock/skill_export.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

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

bool runProvider(std::vector<std::string> args, std::string_view input = {}) {
    // A private, unnamed file lets the child read the configuration without putting it in argv.
    std::unique_ptr<FILE,decltype(&std::fclose)> data(nullptr,std::fclose);
    if (!input.empty()) {
        data.reset(std::tmpfile());
        if (!data || std::fwrite(input.data(),1,input.size(),data.get()) != input.size() ||
            std::fflush(data.get()) != 0 || std::fseek(data.get(),0,SEEK_SET) != 0 ||
            fcntl(fileno(data.get()),F_SETFD,FD_CLOEXEC) < 0) return false;
    }
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    const auto redirect = posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0) ||
                          posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0) ||
                          (data && posix_spawn_file_actions_adddup2(&actions,fileno(data.get()),STDIN_FILENO));
    pid_t pid = 0;
    const auto started = redirect == 0 && posix_spawnp(&pid,argv.front(),&actions,nullptr,argv.data(),environ) == 0;
    posix_spawn_file_actions_destroy(&actions);
    if (!started) return false;
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid,&status,0); } while (waited < 0 && errno == EINTR);
    return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
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

void editCodex(std::string_view url, std::string_view token) {
    constexpr std::string_view begin = "# BEGIN Flock T3 connection\n";
    constexpr std::string_view end = "# END Flock T3 connection\n";
    const auto path = userPath("FLOCK_CODEX_CONFIG",".codex/config.toml");
    if (fs::is_symlink(path)) throw Conflict("Use FLOCK_CODEX_CONFIG to point to the Codex configuration file");
    std::ifstream stream(path,std::ios::binary);
    if (!stream && fs::exists(path)) throw Conflict("Could not read the Codex configuration");
    std::string config(std::istreambuf_iterator<char>(stream),{});
    const auto original = config;
    const auto start = config.find(begin);
    if (start != config.npos) {
        const auto finish = config.find(end,start + begin.size());
        if (finish == config.npos) throw Conflict("Incomplete Flock entry in Codex configuration");
        config.erase(start,finish + end.size() - start);
    }
    if (!url.empty()) {
        // The server name is reserved for this connector.
        if (config.find("[mcp_servers.flock]") != config.npos || config.find("[mcp_servers.\"flock\"]") != config.npos ||
            config.find("[mcp_servers.'flock']") != config.npos)
            throw Conflict("Remove the existing Codex MCP server named flock before connecting");
        if (!config.empty() && config.back() != '\n') config += '\n';
        config += begin;
        config += "[mcp_servers.flock]\nurl = " + Json(url).dump() +
                  "\nhttp_headers = { Authorization = " + Json("Bearer " + std::string(token)).dump() + " }\n";
        config += end;
    }
    if (original != config) saveConfig(path,config);
}

void removeClaude(const std::string& command) {
    if (!runProvider({command,"mcp","remove","--scope","user","flock"}) &&
        runProvider({command,"mcp","get","flock"}))
        throw Conflict("Could not remove the Claude Code MCP server");
}

}

T3SetupLock::T3SetupLock(const std::filesystem::path& state) {
    fs::create_directories(state);
    fd_ = open((state / ".t3-setup.lock").c_str(),O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,0600);
    if (fd_ < 0) throw Conflict("Could not open the T3 setup lock");
    if (flock(fd_,LOCK_EX | LOCK_NB) != 0) {
        close(fd_);
        throw Conflict("Another T3 setup command is running");
    }
}

T3SetupLock::~T3SetupLock() { close(fd_); }

Json configureT3Providers(std::string_view url, std::string_view token) {
    auto providers = Json::array();
    if (installed(setting("FLOCK_CODEX_CLI","codex"))) {
        editCodex(url,token);
        providers.push_back("codex");
    }
    const auto claude = setting("FLOCK_CLAUDE_CLI","claude");
    if (installed(claude)) {
        runProvider({claude,"mcp","remove","--scope","user","flock"});
        const Json server{{"type","http"},{"url",url},{"headers",{{"Authorization","Bearer " + std::string(token)}}}};
        if (!runProvider({claude,"mcp","add-json","--scope","user","flock","-"},server.dump()))
            throw Conflict("Could not configure the Claude Code MCP server. Claude Code 2.1.327 or later is required.");
        providers.push_back("claude");
    }
    return providers;
}

void removeT3Providers() {
    editCodex({},{});
    const auto claude = setting("FLOCK_CLAUDE_CLI","claude");
    if (installed(claude)) removeClaude(claude);
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
