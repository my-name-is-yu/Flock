#include "storage.hpp"
#include "workspace_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace flockv0 {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace {
constexpr std::size_t maxEntries = 2048;
constexpr std::size_t maxSearchBytes = 1024 * 1024;
constexpr std::size_t maxFileBytes = 8192;
constexpr auto supervisorDeadline = 12s;

class Fd {
    int value_;
public:
    explicit Fd(int value) : value_(value) {
        if (value < 0) throw Conflict("Workspace path or descriptor is unavailable");
    }
    Fd(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_,-1)) {}
    Fd& operator=(Fd&& other) noexcept {
        std::swap(value_,other.value_);
        return *this;
    }
    ~Fd() { if (value_ >= 0) close(value_); }
    int get() const { return value_; }
    void reset() { if (value_ >= 0) close(std::exchange(value_,-1)); }
};

bool within(const fs::path& root, const fs::path& child) {
    const auto [parent,end] = std::mismatch(root.begin(),root.end(),child.begin(),child.end());
    return parent == root.end() && end != child.end();
}

void checkPath(const fs::path& root, const fs::path& relative) {
    if (!within(root,fs::weakly_canonical(root / relative)))
        throw Conflict("Path leaves the authorized directory");
}

Fd directory(const fs::path& root, const fs::path& relative) {
    Fd current(open(root.c_str(),O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    for (const auto& part : relative) {
        current = Fd(openat(current.get(),part.c_str(),O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    }
    return current;
}

std::string readFile(const fs::path& root, const fs::path& relative, std::size_t cap) {
    checkPath(root,relative);
    const auto parent = directory(root,relative.parent_path());
    const Fd file(openat(parent.get(),relative.filename().c_str(),O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (fstat(file.get(),&info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
        throw Conflict("Only workspace regular files are readable");
    std::string result(cap,'\0');
    std::size_t size = 0;
    while (size < cap) {
        const auto n = read(file.get(),result.data()+size,cap-size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw Conflict("Unable to read workspace file");
        if (n == 0) break;
        size += static_cast<std::size_t>(n);
    }
    result.resize(size);
    return result;
}

void writeFile(const fs::path& root, const fs::path& relative, std::string_view data) {
    checkPath(root,relative);
    const auto parent = directory(root,relative.parent_path());
    const auto filename = relative.filename();
    struct stat existing{};
    if (fstatat(parent.get(),filename.c_str(),&existing,AT_SYMLINK_NOFOLLOW) == 0 &&
        (!S_ISREG(existing.st_mode) || existing.st_nlink != 1))
        throw Conflict("Only workspace regular files can be replaced");
    const auto temporary = ".flock-write-" + actionId();
    const Fd file(openat(parent.get(),temporary.c_str(),O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,0600));
    try {
        while (!data.empty()) {
            const auto count = write(file.get(),data.data(),data.size());
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw Conflict("Unable to write workspace file");
            data.remove_prefix(static_cast<std::size_t>(count));
        }
        if (fsync(file.get()) != 0 || renameat(parent.get(),temporary.c_str(),parent.get(),filename.c_str()) != 0)
            throw Conflict("Unable to save workspace file");
    } catch (...) {
        unlinkat(parent.get(),temporary.c_str(),0);
        throw;
    }
}

bool unsafeEntry(const fs::directory_entry& entry) {
    const auto type = entry.symlink_status().type();
    return type != fs::file_type::directory &&
           (type != fs::file_type::regular || entry.hard_link_count() != 1);
}

Json searchWorkspace(const fs::path& root, const fs::path& relative, std::string_view query) {
    auto base = root;
    if (!relative.empty()) {
        checkPath(root,relative);
        directory(root,relative);
        base /= relative;
    }
    auto files = Json::array();
    std::size_t visited = 0, bytes = 0;
    bool truncated = false;
    for (const auto& entry : fs::recursive_directory_iterator(base)) {
        if (++visited > maxEntries || bytes >= maxSearchBytes || files.size() >= 40) {
            truncated = true;
            break;
        }
        if (unsafeEntry(entry) || !entry.is_regular_file()) continue;
        const auto requested = entry.path().lexically_relative(root);
        const auto content = readFile(root,requested,std::min(maxFileBytes+1,maxSearchBytes-bytes));
        bytes += content.size();
        truncated |= content.size() > maxFileBytes;
        if (content.find(query) != std::string::npos) files.push_back(requested.generic_string());
    }
    return {{"files",files},{"truncated",truncated}};
}

void ensureDirectory(const fs::path& path) {
    if (mkdir(path.c_str(),0700) != 0 && errno != EEXIST) throw Conflict("Cannot create Bot workspace");
    directory(path,{});
}

fs::path workspaceRoot(const fs::path& database, const ToolContext& ctx, bool create) {
    if (!validBotId(ctx.bot)) throw Conflict("Invalid Bot workspace");
    const auto state = fs::canonical(database.has_parent_path() ? database.parent_path() : fs::path("."));
    const auto configured = ctx.settings.workspaceRoot.empty() ? state / "workspaces" : ctx.settings.workspaceRoot;
    const auto base = fs::weakly_canonical(fs::absolute(configured));
    const auto actualDatabase = fs::weakly_canonical(database);
    if (base == state || within(base,state) || within(base,actualDatabase) ||
        (within(state,base) && base != state / "workspaces") ||
        fs::is_symlink(configured))
        throw Conflict("Workspace storage must be isolated from Flock state");
    if (create) ensureDirectory(base);
    const auto root = base / ctx.bot;
    if (fs::is_symlink(root) || fs::weakly_canonical(root) != root) throw Conflict("Unsafe Bot workspace directory");
    if (create) ensureDirectory(root);
    return root;
}

std::vector<std::string> commandArguments(const Json& args) {
    const auto& argv = args.at("argv");
    if (!argv.is_array() || argv.empty() || argv.size() > 32) throw Conflict("Expected 1 to 32 command arguments");
    std::vector<std::string> result;
    for (const auto& value : argv) {
        if (!value.is_string() || value.get_ref<const std::string&>().size() > 4096 ||
            value.get_ref<const std::string&>().find('\0') != std::string::npos)
            throw Conflict("Invalid command argument");
        result.push_back(value.get<std::string>());
    }
    if (result.front().empty() || result.front().starts_with('-')) throw Conflict("Invalid executable");
    return result;
}

fs::path executable(const fs::path& root, const std::string& name) {
    const fs::path requested(name);
    if (requested.is_absolute()) {
        if (requested.parent_path() != "/bin" && requested.parent_path() != "/usr/bin")
            throw Conflict("Executable must be in the workspace or system bin");
        return requested;
    }
    if (!requested.has_parent_path()) {
        for (const auto* prefix : {"/usr/bin","/bin"}) {
            const auto candidate = fs::path(prefix) / requested;
            if (access(candidate.c_str(),X_OK) == 0) return candidate;
        }
    }
    const auto relative = relativeToolPath(name);
    checkPath(root,relative);
    return root / relative;
}

#ifdef __APPLE__
std::string sandboxLiteral(std::string_view path) {
    std::string result = "\"";
    for (const auto ch : path) {
        if (static_cast<unsigned char>(ch) < 32 || ch == 127) throw Conflict("Unsafe workspace path");
        if (ch == '\\' || ch == '"') result += '\\';
        result += ch;
    }
    return result + '"';
}
#endif

const std::array systemRoots{
#if defined(__APPLE__)
    "/usr/lib","/System/Library","/usr/share/locale","/usr/bin","/bin"
#else
    "/usr/bin","/usr/lib","/usr/lib64","/bin","/lib","/lib64"
#endif
};

std::vector<std::string> sandboxCommand(const fs::path& root, const fs::path& cwd, const Json& args, bool writable,
                                        const fs::path& database) {
    const auto state = fs::canonical(database.has_parent_path() ? database.parent_path() : fs::path("."));
    const auto actualDatabase = fs::weakly_canonical(database);
    for (const auto* directory : systemRoots) {
        if (!fs::exists(directory)) continue;
        const auto system = fs::canonical(directory);
        if (system == state || within(system,state) || within(system,root) || within(system,actualDatabase))
            throw Conflict("Flock state and Bot workspaces must be outside sandbox system directories");
    }
    auto command = commandArguments(args);
    command.front() = executable(root,command.front()).string();
#if defined(__APPLE__)
    const auto workspace = "(subpath " + sandboxLiteral(root.string()) + ")";
    auto profile = std::string(R"PROFILE((version 1)
(deny default)
(allow file-read* file-map-executable (subpath "/usr/lib") (subpath "/System/Library") (subpath "/usr/share/locale")
                 (subpath "/usr/bin") (subpath "/bin") (literal "/dev/null"))
(allow file-read-data (literal "/"))
(allow process-exec (subpath "/usr/bin") (subpath "/bin"))
(allow sysctl-read (sysctl-name "kern.osrelease") (sysctl-name "hw.ncpu"))
)PROFILE") + "(allow file-read* file-map-executable " + workspace + ")\n(allow process-exec " + workspace + ")\n";
    for (auto parent = cwd.parent_path(); !parent.empty() && parent != parent.root_path(); parent = parent.parent_path())
        profile += "(allow file-read-metadata (literal " + sandboxLiteral(parent.string()) + "))\n";
    if (writable) profile += "(allow file-write* " + workspace + ")\n";
    std::vector<std::string> launch{"/usr/bin/sandbox-exec","-p",std::move(profile),"--"};
#elif defined(__linux__)
    const auto inner = [](const fs::path& rootPath, const fs::path& path) {
        return std::string("/workspace/") + path.lexically_relative(rootPath).generic_string();
    };
    if (within(root,command.front())) command.front() = inner(root,command.front());
    std::vector<std::string> launch{"/usr/bin/bwrap","--unshare-all","--die-with-parent",
        "--new-session","--disable-userns","--cap-drop","ALL","--dev","/dev"};
    for (const auto* path : systemRoots) if (fs::exists(path))
        launch.insert(launch.end(),{"--ro-bind",path,path});
    launch.insert(launch.end(),{writable ? "--bind" : "--ro-bind",root.string(),"/workspace",
                               "--chdir",inner(root,cwd),"--"});
#else
    (void)cwd;
    (void)writable;
    throw Conflict("No supported command sandbox on this host");
    std::vector<std::string> launch;
#endif
    launch.insert(launch.begin(),FLOCK_WORKSPACE_SUPERVISOR);
    launch.insert(launch.end(),command.begin(),command.end());
    return launch;
}

struct Spawn {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    Spawn() {
        check(posix_spawn_file_actions_init(&actions));
        if (posix_spawnattr_init(&attributes) != 0) {
            posix_spawn_file_actions_destroy(&actions);
            throw Conflict("Cannot configure sandbox process");
        }
    }
    ~Spawn() {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
    }
    static void check(int error) {
        if (error) throw Conflict("Cannot start isolated workspace command");
    }
};

pid_t launchSandbox(const fs::path& cwd, std::vector<std::string> command, int lease, int out, int err) {
    Spawn spawn;
    Spawn::check(posix_spawn_file_actions_adddup2(&spawn.actions,lease,STDIN_FILENO));
    Spawn::check(posix_spawn_file_actions_adddup2(&spawn.actions,out,STDOUT_FILENO));
    Spawn::check(posix_spawn_file_actions_adddup2(&spawn.actions,err,STDERR_FILENO));
#ifdef __APPLE__
    Spawn::check(posix_spawnattr_setflags(&spawn.attributes,POSIX_SPAWN_CLOEXEC_DEFAULT));
    const auto working = directory(cwd,{});
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    Spawn::check(posix_spawn_file_actions_addfchdir_np(&spawn.actions,working.get()));
#pragma clang diagnostic pop
    auto home = "HOME=" + cwd.string();
#else
    (void)cwd;
    Spawn::check(posix_spawn_file_actions_addclosefrom_np(&spawn.actions,3));
    std::string home = "HOME=/workspace";
#endif
    std::string path = "PATH=/usr/bin:/bin", lang = "LANG=C";
    char* env[]{home.data(),path.data(),lang.data(),nullptr};
    std::vector<char*> argv;
    for (auto& arg : command) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    Spawn::check(posix_spawn(&pid,argv.front(),&spawn.actions,&spawn.attributes,argv.data(),env));
    return pid;
}

struct Pipe {
    static std::pair<Fd,Fd> make() {
        int fds[2];
#if defined(__linux__)
        if (pipe2(fds,O_CLOEXEC) != 0) throw Conflict("Cannot create command output pipe");
#else
        if (pipe(fds) != 0) throw Conflict("Cannot create command output pipe");
#endif
        std::pair<Fd,Fd> result{Fd(fds[0]),Fd(fds[1])};
        if (fcntl(fds[0],F_SETFD,FD_CLOEXEC) < 0 || fcntl(fds[1],F_SETFD,FD_CLOEXEC) < 0 ||
            fcntl(fds[0],F_SETFL,O_NONBLOCK) < 0)
            throw Conflict("Cannot configure command output");
        return result;
    }
    std::pair<Fd,Fd> ends = make();
};

struct Capture {
    std::string output[2];
    bool truncated = false;
    bool readAvailable(int fd, unsigned channel) {
        char buffer[4096];
        bool changed = false;
        for (int i = 0; i < 8; ++i) {
            const auto n = read(fd,buffer,sizeof(buffer));
            if (n <= 0) break;
            const auto room = workspaceOutputLimit-output[0].size()-output[1].size();
            const auto used = std::min(static_cast<std::size_t>(n),room);
            changed |= used > 0 || !truncated;
            output[channel].append(buffer,used);
            truncated |= used < static_cast<std::size_t>(n);
        }
        return changed;
    }
};

void verifyTree(const fs::path& root) {
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (++count > maxEntries) throw Conflict("Workspace has too many entries for command execution");
        if (unsafeEntry(entry)) throw Conflict("Command workspace contains an unsafe entry");
    }
}

std::string safeOutput(std::string_view value) {
    return Json::parse(Json(std::string(value)).dump(-1,' ',false,Json::error_handler_t::ignore)).get<std::string>();
}
}

fs::path relativeToolPath(std::string_view value) {
    fs::path path(value);
    if (value.empty() || value.size() > 1024 || value.find('\0') != value.npos || path.is_absolute() ||
        path.filename().empty() || std::ranges::any_of(path,[](const auto& part) { return part == ".."; }))
        throw Conflict("Expected a relative path inside the authorized directory");
    return path.lexically_normal();
}

Json readToolFile(const fs::path& root, const fs::path& relative) {
    auto data = readFile(root,relative,maxFileBytes+1);
    const auto truncated = data.size() > maxFileBytes;
    if (truncated) data.resize(maxFileBytes);
    return {{"text",safeOutput(data)},{"truncated",truncated}};
}

void Store::Impl::migrateWorkspace() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS workspace_commands(
 id INTEGER PRIMARY KEY, bot TEXT NOT NULL, started_at INTEGER NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('running','stop_requested','exited','unknown_after_restart')),
 stdout TEXT NOT NULL DEFAULT '', stderr TEXT NOT NULL DEFAULT '', truncated INTEGER NOT NULL DEFAULT 0,
 exit_code INTEGER);
)SQL");
}

void Store::Impl::recoverWorkspace() {
    std::lock_guard lock(mutex);
    exec("UPDATE workspace_commands SET state='unknown_after_restart' WHERE state IN ('running','stop_requested')");
}

void Store::Impl::stopWorkspace() {
    std::map<Id,std::unique_ptr<WorkspaceJob>> running;
    {
        std::lock_guard lock(mutex);
        running.swap(workspaceJobs);
        for (auto& [id,job] : running) job->worker.request_stop();
    }
}

void Store::Impl::validateWorkspace(const ToolContext& ctx, const Decision& call) {
    const auto& args = call.args;
    const auto root = workspaceRoot(path,ctx,false);
    if (call.tool == "workspace_command_status" || call.tool == "workspace_stop_command") {
        if (!query("SELECT 1 FROM workspace_commands WHERE bot=? AND id=?",
                   {ctx.bot,toolId(args,"commandId")}).next()) throw Conflict("Unknown workspace command");
        return;
    }
    if (call.tool == "workspace_run_command") {
        if (!Access(*this).permits(ctx.bot,{"workspace",ctx.bot,"read"},true))
            throw Conflict("Workspace command requires workspace read access");
        const auto cwd = args.contains("cwd") ? relativeToolPath(toolText(args,"cwd",1024)) : fs::path{};
        if (!cwd.empty() && cwd != ".") checkPath(root,cwd);
        executable(root,commandArguments(args).front());
        return;
    }
    if (call.tool != "workspace_search" || args.contains("path")) {
        const auto relative = relativeToolPath(toolText(args,"path",1024));
        if (relative == "." && call.tool != "workspace_search") throw Conflict("Expected a workspace file");
        if (relative != ".") checkPath(root,relative);
    }
}

Json Store::Impl::workspaceTool(const ToolContext& ctx, const Decision& call) {
    const auto& args = call.args;
    if (call.tool == "workspace_command_status" || call.tool == "workspace_stop_command") {
        std::lock_guard lock(mutex);
        const auto id = toolId(args,"commandId");
        if (call.tool == "workspace_stop_command") {
            exec("UPDATE workspace_commands SET state='stop_requested' WHERE bot=? AND id=? AND state='running'",{ctx.bot,id});
            if (sqlite3_changes(db)) {
                const auto job = workspaceJobs.find(id);
                if (job != workspaceJobs.end()) job->second->stop = true;
            }
        }
        auto row = query("SELECT state,stdout,stderr,truncated,coalesce(exit_code,-1) "
                         "FROM workspace_commands WHERE bot=? AND id=?",{ctx.bot,id});
        if (!row.next()) throw Conflict("Unknown workspace command");
        const auto exit = row.number(4);
        return {{"commandId",id},{"state",row.string(0)},{"stdout",row.string(1)},{"stderr",row.string(2)},
                {"truncated",row.number(3)!=0},{"exitCode",exit < 0 ? Json(nullptr) : Json(exit)}};
    }
    const auto root = workspaceRoot(path,ctx,true);
    if (call.tool == "workspace_search") {
        auto relative = args.contains("path") ? relativeToolPath(toolText(args,"path",1024)) : fs::path{};
        if (relative == ".") relative.clear();
        return searchWorkspace(root,relative,toolText(args,"query",256));
    }
    if (call.tool == "workspace_read_file") return readToolFile(root,relativeToolPath(toolText(args,"path",1024)));
    if (call.tool == "workspace_write_file") {
        const auto data = toolText(args,"text",maxFileBytes,true);
        writeFile(root,relativeToolPath(toolText(args,"path",1024)),data);
        return {{"bytes",data.size()}};
    }

    auto relative = args.contains("cwd") ? relativeToolPath(toolText(args,"cwd",1024)) : fs::path{};
    if (relative == ".") relative.clear();
    if (!relative.empty()) checkPath(root,relative);
    directory(root,relative);
    verifyTree(root);
    const auto cwd = relative.empty() ? root : root / relative;
    const bool writable = Access(*this).permits(ctx.bot,{"workspace",ctx.bot,"write"},true);
    auto command = sandboxCommand(root,cwd,args,writable,path);
    Pipe out, err, lease;
    std::lock_guard lock(mutex);
    std::erase_if(workspaceJobs,[](const auto& entry) { return entry.second->done.load(); });
    if (workspaceJobs.size() >= 8 || query("SELECT 1 FROM workspace_commands WHERE bot=? AND state IN "
                                          "('running','stop_requested')",{ctx.bot}).next())
        throw Conflict("Workspace command limit reached");
    auto job = std::make_unique<WorkspaceJob>();
    auto* control = job.get();
    exec("INSERT INTO workspace_commands(bot,started_at,state) VALUES(?,?,'running')",{ctx.bot,unixTime()});
    const auto id = last();
    auto [entry,inserted] = workspaceJobs.emplace(id,std::move(job));
    (void)inserted;
    try {
        control->worker = std::jthread([this,control,id,cwd,command = std::move(command),
                                        out = std::move(out),err = std::move(err),lease = std::move(lease)](std::stop_token shutdown) mutable {
            pid_t pid = 0;
            try {
                pid = launchSandbox(cwd,std::move(command),lease.ends.first.get(),out.ends.second.get(),err.ends.second.get());
                lease.ends.first.reset();
                out.ends.second.reset();
                err.ends.second.reset();
                auto readers = std::array{pollfd{out.ends.first.get(),POLLIN,0},pollfd{err.ends.first.get(),POLLIN,0}};
                Capture capture;
                const auto deadline = std::chrono::steady_clock::now() + supervisorDeadline;
                while (true) {
                    if (control->stop || shutdown.stop_requested()) lease.ends.second.reset();
                    if (std::chrono::steady_clock::now() >= deadline) throw Conflict("Sandbox supervisor exceeded its deadline");
                    const auto polled = poll(readers.data(),readers.size(),30);
                    if (polled < 0 && errno != EINTR) throw Conflict("Cannot capture sandbox output");
                    bool changed = false;
                    for (unsigned i = 0; i < readers.size(); ++i) {
                        changed |= capture.readAvailable(readers[i].fd,i);
                        if (readers[i].revents & POLLHUP) readers[i].fd = -1;
                    }
                    int status = 0;
                    const auto child = waitpid(pid,&status,WNOHANG);
                    if (child < 0 && errno != EINTR) throw Conflict("Cannot observe sandbox process");
                    if (child > 0) {
                        pid = 0;
                        for (unsigned i = 0; i < readers.size(); ++i) capture.readAvailable(readers[i].fd,i);
                    }
                    if (shutdown.stop_requested() && child > 0) break;
                    if (changed || child > 0) {
                        std::lock_guard guard(mutex);
                        const auto exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                        exec("UPDATE workspace_commands SET stdout=?,stderr=?,truncated=?,"
                             "state=CASE WHEN ? THEN 'exited' ELSE state END,exit_code=? WHERE id=?",
                             {safeOutput(capture.output[0]),safeOutput(capture.output[1]),int(capture.truncated),
                              int(child > 0),child > 0 ? Json(exit) : Json(nullptr),id});
                    }
                    if (child > 0) break;
                }
            } catch (const std::exception&) {
                lease.ends.second.reset();
                if (pid > 0) {
                    while (waitpid(pid,nullptr,0) < 0 && errno == EINTR) {}
                }
                try {
                    std::lock_guard guard(mutex);
                    exec("UPDATE workspace_commands SET state='exited',exit_code=125,"
                         "stderr='Sandbox command or supervisor failed' WHERE id=?",{id});
                } catch (const std::exception&) {}
            }
            control->done = true;
        });
    } catch (...) {
        workspaceJobs.erase(entry);
        exec("UPDATE workspace_commands SET state='exited',exit_code=125 WHERE id=?",{id});
        throw;
    }
    return {{"commandId",id},{"state","running"}};
}
}
