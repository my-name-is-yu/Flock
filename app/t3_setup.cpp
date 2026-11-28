#include "t3_setup.hpp"
#include "flock/environment.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

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
    if (command.find('/') != command.npos) return access(command.c_str(), X_OK) == 0;
    auto paths = setting("PATH");
    std::size_t start = 0;
    while (start < paths.size()) {
        const auto end = paths.find(':', start);
        const auto directory = paths.substr(start, end - start);
        if (!directory.empty() && access((fs::path(directory) / command).c_str(), X_OK) == 0) return true;
        if (end == paths.npos) break;
        start = end + 1;
    }
    return false;
}

bool runProvider(std::vector<std::string> args, std::string_view input = {}, std::string* output = nullptr) {
    // A private, unnamed file lets the child read the configuration without putting it in argv.
    std::unique_ptr<FILE, decltype(&std::fclose)> data(nullptr, std::fclose);
    if (!input.empty()) {
        data.reset(std::tmpfile());
        if (!data || std::fwrite(input.data(), 1, input.size(), data.get()) != input.size() || std::fflush(data.get()) != 0 ||
            std::fseek(data.get(), 0, SEEK_SET) != 0 || fcntl(fileno(data.get()), F_SETFD, FD_CLOEXEC) < 0)
            return false;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> capture(nullptr, std::fclose);
    if (output) {
        capture.reset(std::tmpfile());
        if (!capture || fcntl(fileno(capture.get()), F_SETFD, FD_CLOEXEC) < 0) return false;
    }
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    const auto redirect = (capture ? posix_spawn_file_actions_adddup2(&actions, fileno(capture.get()), STDOUT_FILENO)
                                   : posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0)) ||
                          posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) ||
                          (data && posix_spawn_file_actions_adddup2(&actions, fileno(data.get()), STDIN_FILENO));
    pid_t pid = 0;
    const auto started = redirect == 0 && posix_spawnp(&pid, argv.front(), &actions, nullptr, argv.data(), environ) == 0;
    posix_spawn_file_actions_destroy(&actions);
    if (!started) return false;
    int status = 0;
    pid_t waited;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        waited = waitpid(pid, &status, WNOHANG);
        if (waited < 0 && errno == EINTR) continue;
        if (waited != 0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            do {
                waited = waitpid(pid, &status, 0);
            } while (waited < 0 && errno == EINTR);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    if (capture) {
        char line[128];
        if (std::fseek(capture.get(), 0, SEEK_SET) != 0 || !std::fgets(line, sizeof line, capture.get())) return false;
        *output = line;
    }
    return true;
}

void saveConfig(const fs::path& path, std::string_view content) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    auto temporary = path.string() + ".flock.XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) throw Conflict("Could not create a private Codex configuration file");
    FILE* output = fdopen(fd, "wb");
    if (!output) {
        close(fd);
        fs::remove(temporary);
        throw Conflict("Could not open the Codex configuration for writing");
    }
    const bool written = std::fwrite(content.data(), 1, content.size(), output) == content.size();
    const auto closed = std::fclose(output);
    std::error_code error;
    if (written && closed == 0) fs::rename(temporary, path, error);
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
        std::ifstream stream(path, std::ios::binary);
        if (!stream && fs::exists(path)) throw Conflict("Could not read provider configuration");
        if (stream) original.emplace(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        content = original.value_or("");
    }

    bool changed() const { return content != original.value_or(""); }
    void apply() const {
        if (changed()) saveConfig(path, content);
    }
    void restore() const {
        if (!changed()) return;
        if (original)
            saveConfig(path, *original);
        else
            fs::remove(path);
    }
};

ConfigEdit editCodex(std::string_view url, std::string_view token, const OwnsBearer& owns) {
    constexpr std::string_view begin = "# BEGIN Flock T3 connection\n";
    constexpr std::string_view end = "# END Flock T3 connection\n";
    ConfigEdit edit(userPath(environment::codexConfig, ".codex/config.toml"));
    auto& config = edit.content;
    const auto start = config.find(begin);
    if (start != config.npos) {
        const auto finish = config.find(end, start + begin.size());
        if (finish == config.npos) throw Conflict("Incomplete Flock entry in Codex configuration");
        if (url.empty() || owns) {
            const auto parsed = toml::parse(config);
            const auto header = parsed["mcp_servers"]["flock"]["http_headers"]["Authorization"].value_or(std::string{});
            if (!matchesBearer(header, owns)) {
                if (url.empty()) return edit;
                throw Conflict("The Codex MCP registration belongs to another Flock installation");
            }
        }
        config.erase(start, finish + end.size() - start);
    }
    if (!url.empty()) {
        try {
            const auto parsed = toml::parse(config);
            if (parsed["mcp_servers"]["flock"]) throw Conflict("Remove the existing Codex MCP server named flock before connecting");
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

std::optional<Json> readClaudeServer() {
    const auto directory = setting("CLAUDE_CONFIG_DIR");
    const auto path = directory.empty() ? userPath(environment::claudeConfig, ".claude.json") : fs::path(directory) / ".claude.json";
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (fs::exists(path)) throw Conflict("Could not read Claude Code configuration");
        return {};
    }
    const auto config = Json::parse(stream);
    if (!config.is_object()) throw Conflict("Invalid Claude Code configuration");
    const auto servers = config.find("mcpServers");
    if (servers == config.end() || servers->is_null()) return {};
    if (!servers->is_object()) throw Conflict("Invalid Claude Code MCP configuration");
    const auto server = servers->find("flock");
    if (server == servers->end()) return {};
    return *server;
}

bool ownsClaudeServer(const std::optional<Json>& server, const OwnsBearer& owns) {
    if (!server || !server->is_object()) return false;
    const auto headers = server->find("headers");
    if (headers == server->end() || !headers->is_object()) return false;
    const auto header = headers->find("Authorization");
    return header != headers->end() && header->is_string() && matchesBearer(header->get_ref<const std::string&>(), owns);
}

constexpr auto claudeSetupError = "Could not configure the Claude Code MCP server. Claude Code 2.1.327 or later is required.";

void requireClaudeStdin(const std::string& cli) {
    std::string version;
    int major, minor, patch;
    if (!runProvider({cli, "--version"}, {}, &version) || std::sscanf(version.c_str(), "%d.%d.%d", &major, &minor, &patch) != 3 ||
        std::tuple(major, minor, patch) < std::tuple(2, 1, 327))
        throw Conflict(claudeSetupError);
}

void addClaude(const std::string& cli, const Json& server) {
    if (!runProvider({cli, "mcp", "add-json", "--scope", "user", "flock", "-"}, server.dump())) throw Conflict(claudeSetupError);
}

void removeClaude(const std::string& cli) {
    if (!runProvider({cli, "mcp", "remove", "--scope", "user", "flock"})) throw Conflict("Could not remove the Claude Code MCP server");
}

void restoreClaude(const std::string& cli, const std::optional<Json>& previous, const Json& replacement) {
    const auto current = readClaudeServer();
    if (current == previous) return;
    if (current) {
        if (*current != replacement) throw Conflict("Claude Code MCP registration changed during T3 setup");
        removeClaude(cli);
    }
    if (previous) addClaude(cli, *previous);
}

}

T3SetupLock::T3SetupLock() {
    const auto directory = userPath("HOME", "") / ".codex";
    fs::create_directories(directory);
    fd_ = open((directory / ".t3-setup.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) throw Conflict("Could not open the T3 setup lock");
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        close(fd_);
        throw Conflict("Another T3 setup command is running");
    }
}

T3SetupLock::~T3SetupLock() {
    close(fd_);
}

std::vector<std::string> configureT3Providers(std::string_view url, const std::function<void(const InstallT3Provider&)>& connect,
                                              const OwnsBearer& owns) {
    std::vector<std::string> providers;
    std::optional<ConfigEdit> codex;
    const auto claude = setting(environment::claudeCli, "claude");
    std::optional<Json> previousClaude;
    bool codexApplied = false;
    std::optional<Json> replacementClaude;
    try {
        connect([&](std::string_view token) {
            if (installed(setting(environment::codexCli, "codex"))) {
                codex = editCodex(url, token, owns);
                providers.emplace_back("codex");
            }
            const bool hasClaude = installed(claude);
            if (hasClaude) {
                previousClaude = readClaudeServer();
                if (previousClaude && !ownsClaudeServer(previousClaude, [&](std::string_view bearer) {
                        return owns ? owns(bearer) : bearer.starts_with("flock-t3-");
                    }))
                    throw Conflict("Remove the existing Claude Code MCP server named flock before connecting");
                if (previousClaude) requireClaudeStdin(claude);
                providers.emplace_back("claude");
            }
            if (codex) {
                codex->apply();
                codexApplied = true;
            }
            if (hasClaude) {
                replacementClaude = Json{{"type", "http"}, {"url", url}, {"headers", {{"Authorization", "Bearer " + std::string(token)}}}};
                if (previousClaude) removeClaude(claude);
                addClaude(claude, *replacementClaude);
            }
        });
    } catch (...) {
        bool rollbackFailed = false;
        if (replacementClaude) {
            try {
                restoreClaude(claude, previousClaude, *replacementClaude);
            } catch (...) {
                rollbackFailed = true;
            }
        }
        if (codexApplied) {
            try {
                codex->restore();
            } catch (...) {
                rollbackFailed = true;
            }
        }
        if (rollbackFailed) throw Conflict("Could not restore provider configuration after T3 setup failed");
        throw;
    }
    return providers;
}

bool t3ProvidersNeedRefresh(const OwnsBearer& owns) {
    if (installed(setting(environment::codexCli, "codex")) && !editCodex({}, {}, owns).changed()) return true;
    return installed(setting(environment::claudeCli, "claude")) && !ownsClaudeServer(readClaudeServer(), owns);
}

void removeT3Providers(const OwnsBearer& owns) {
    const auto codex = editCodex({}, {}, owns);
    const auto claude = setting(environment::claudeCli, "claude");
    // Without Claude Code there is no registration to remove, and its leftover file must not block Codex cleanup.
    const bool removeRegistration = installed(claude) && ownsClaudeServer(readClaudeServer(), owns);
    codex.apply();
    try {
        if (removeRegistration) removeClaude(claude);
    } catch (...) {
        codex.restore();
        throw;
    }
}

}
