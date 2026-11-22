#include "storage.hpp"
#include "workspace_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <dirent.h>
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
constexpr std::size_t maxPipelineStages = 4;
constexpr std::size_t maxPipelineBytes = 32768;
constexpr auto supervisorDeadline = 12s;
constexpr std::array<std::string_view,14> pipelineExecutables{
    "cat","cut","echo","grep","head","printf","sleep","sort","tail","tee","tr","uniq","wc","yes"
};

std::string safeOutput(std::string_view value) {
    return Json::parse(Json(std::string(value)).dump(-1,' ',false,Json::error_handler_t::replace)).get<std::string>();
}

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
    int release() { return std::exchange(value_,-1); }
    void reset() { if (value_ >= 0) close(std::exchange(value_,-1)); }
};

Fd directoryAt(int parent, const fs::path& relative) {
    Fd current(openat(parent,".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    for (const auto& part : relative)
        current = Fd(openat(current.get(),part.c_str(),O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    return current;
}

Fd directory(const fs::path& root) {
    const Fd slash(open("/",O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    return directoryAt(slash.get(),root.relative_path());
}

template<class Visit>
bool visitDirectory(int parent, const Visit& visit) {
    auto fd = directoryAt(parent,{});
    const std::unique_ptr<DIR,decltype(&closedir)> entries(fdopendir(fd.get()),closedir);
    if (!entries) throw Conflict("Cannot enumerate workspace directory");
    fd.release();
    while (true) {
        errno = 0;
        const auto* entry = readdir(entries.get());
        if (!entry) {
            if (errno) throw Conflict("Cannot enumerate workspace directory");
            return true;
        }
        const std::string_view name(entry->d_name);
        if (name != "." && name != ".." && !visit(name)) return false;
    }
}

Fd regularFile(int parent, const fs::path& filename) {
    Fd file(openat(parent,filename.c_str(),O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (fstat(file.get(),&info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
        throw Conflict("Only regular files with one link are readable");
    return file;
}

std::string readFile(int file, std::size_t cap) {
    std::string result(cap,'\0');
    std::size_t size = 0;
    while (size < cap) {
        const auto n = read(file,result.data()+size,cap-size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw Conflict("Unable to read workspace file");
        if (n == 0) break;
        size += static_cast<std::size_t>(n);
    }
    result.resize(size);
    return result;
}

void writeFile(int root, const fs::path& relative, std::string_view data) {
    const auto parent = directoryAt(root,relative.parent_path());
    const auto filename = relative.filename();
    struct stat existing{};
    mode_t permissions = 0600;
    if (fstatat(parent.get(),filename.c_str(),&existing,AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(existing.st_mode) || existing.st_nlink != 1)
            throw Conflict("Only regular files with one link can be replaced");
        permissions = existing.st_mode & 0777;
    } else if (errno != ENOENT) throw Conflict("Cannot inspect workspace file");

    const auto temporary = ".flock-write-" + actionId();
    const Fd file(openat(parent.get(),temporary.c_str(),O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,0600));
    try {
        while (!data.empty()) {
            const auto count = write(file.get(),data.data(),data.size());
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw Conflict("Unable to write workspace file");
            data.remove_prefix(static_cast<std::size_t>(count));
        }
        if (fchmod(file.get(),permissions) != 0 || fsync(file.get()) != 0 ||
            renameat(parent.get(),temporary.c_str(),parent.get(),filename.c_str()) != 0)
            throw Conflict("Unable to save workspace file");
    } catch (...) {
        unlinkat(parent.get(),temporary.c_str(),0);
        throw;
    }
}

template<class Visit>
bool walk(int parent, const fs::path& prefix, const Visit& visit) {
    return visitDirectory(parent,[&](std::string_view name) {
        const auto relative = prefix / name;
        const auto filename = relative.filename();
        struct stat info{};
        if (fstatat(parent,filename.c_str(),&info,AT_SYMLINK_NOFOLLOW) != 0)
            throw Conflict("Workspace changed while it was being enumerated");
        if (!visit(parent,relative,info)) return false;
        if (!S_ISDIR(info.st_mode)) return true;
        const auto child = directoryAt(parent,filename);
        return walk(child.get(),relative,visit);
    });
}

Json searchWorkspace(int root, const fs::path& relative, std::string_view query) {
    const auto base = directoryAt(root,relative);
    std::vector<std::string> files;
    std::size_t visited = 0, bytes = 0;
    bool truncated = false;
    const bool complete = walk(base.get(),relative,[&](int parent, const fs::path& file, const struct stat& info) {
        if (++visited > maxEntries || bytes >= maxSearchBytes || files.size() >= 40 ||
            file.native().size() > 1024) return false;
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1) return true;
        const auto input = regularFile(parent,file.filename());
        const auto cap = std::min(maxFileBytes,maxSearchBytes-bytes);
        const auto data = readFile(input.get(),cap+1);
        const auto size = std::min(data.size(),cap);
        bytes += size;
        truncated |= data.size() > size;
        if (std::string_view(data).substr(0,size).find(query) != std::string_view::npos) {
            auto name = file.generic_string();
            if (safeOutput(name) == name) files.push_back(std::move(name));
            else truncated = true;
        }
        return true;
    });
    std::ranges::sort(files);
    return {{"files",files},{"truncated",truncated || !complete}};
}

fs::path stateDirectory(const fs::path& database) {
    return fs::canonical(database.has_parent_path() ? database.parent_path() : fs::path("."));
}

struct Workspace {
    fs::path path;
    Fd directory;
};

Workspace openWorkspace(const fs::path& database, const ToolContext& ctx) {
    if (!validBotId(ctx.bot)) throw Conflict("Invalid Bot workspace");
    const auto state = stateDirectory(database);
    const auto basePath = state / "workspaces";
    const auto actualDatabase = fs::canonical(database);
    for (auto parent = actualDatabase.parent_path(); parent != parent.root_path(); parent = parent.parent_path()) {
        if (pathContains(basePath,parent) || (fs::exists(basePath) && fs::equivalent(basePath,parent)))
            throw Conflict("Workspace storage must be outside the Flock database");
    }
    const auto stateFd = directory(state);
    if (mkdirat(stateFd.get(),"workspaces",0700) != 0 && errno != EEXIST)
        throw Conflict("Cannot create workspace storage");
    const Fd base(openExactChildDirectory(stateFd.get(),"workspaces"));
    if (mkdirat(base.get(),ctx.bot.c_str(),0700) != 0 && errno != EEXIST)
        throw Conflict("Cannot create Bot workspace");
    return {basePath / ctx.bot,Fd(openExactChildDirectory(base.get(),ctx.bot))};
}

std::vector<std::string> commandArguments(const Json& argv) {
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

bool nativeExecutable(const fs::path& path) {
    const int fd = open(path.c_str(),O_RDONLY|O_CLOEXEC);
    if (fd < 0) return false;
    unsigned char magic[4]{};
    const auto count = pread(fd,magic,sizeof(magic),0);
    close(fd);
#if defined(__linux__)
    return count == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
#elif defined(__APPLE__)
    return count == 4 && ((magic[0] == 0xfe && magic[1] == 0xed && magic[2] == 0xfa &&
                           (magic[3] == 0xce || magic[3] == 0xcf)) ||
                          (magic[0] == 0xce && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe) ||
                          (magic[0] == 0xcf && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe) ||
                          (magic[0] == 0xca && magic[1] == 0xfe && magic[2] == 0xba &&
                           (magic[3] == 0xbe || magic[3] == 0xbf)) ||
                          (magic[0] == 0xbe && magic[1] == 0xba && magic[2] == 0xfe && magic[3] == 0xca) ||
                          (magic[0] == 0xbf && magic[1] == 0xba && magic[2] == 0xfe && magic[3] == 0xca));
#else
    return false;
#endif
}

fs::path pipelineExecutable(const std::string& name) {
    const fs::path requested(name);
    fs::path candidate;
    if (requested.is_absolute()) {
        if (requested.parent_path() != "/bin" && requested.parent_path() != "/usr/bin")
            throw Conflict("Unsupported pipeline executable; use a T3 Session for scripts or helpers");
        candidate = requested;
    } else if (!requested.has_parent_path()) {
        for (const auto& directory : {fs::path("/usr/bin"),fs::path("/bin")}) {
            const auto current = directory/requested;
            if (access(current.c_str(),X_OK) == 0) {
                candidate = current;
                break;
            }
        }
        if (candidate.empty())
            throw Conflict("Unsupported pipeline executable; use a T3 Session for scripts or helpers");
    } else {
        throw Conflict("Unsupported pipeline executable; use a T3 Session for scripts or helpers");
    }
    std::error_code error;
    const auto resolved = fs::canonical(candidate,error);
    if (error || (resolved.parent_path() != fs::canonical("/bin") &&
                  resolved.parent_path() != fs::canonical("/usr/bin")) ||
        std::ranges::find(pipelineExecutables,resolved.filename().string()) == pipelineExecutables.end() ||
        access(resolved.c_str(),X_OK) != 0 || !fs::is_regular_file(resolved) || !nativeExecutable(resolved))
        throw Conflict("Unsupported pipeline executable; use a T3 Session for scripts or helpers");
    return resolved;
}

std::vector<std::vector<std::string>> pipelineArguments(const Json& pipeline) {
    if (!pipeline.is_array() || pipeline.empty() || pipeline.size() > maxPipelineStages)
        throw Conflict("A pipeline must contain 1 to 4 stages; use a T3 Session for larger workflows");
    std::vector<std::vector<std::string>> stages;
    for (const auto& value : pipeline) {
        auto arguments = commandArguments(value);
        arguments.front() = pipelineExecutable(arguments.front()).string();
        stages.push_back(std::move(arguments));
    }
    return stages;
}

fs::path systemExecutable(const fs::path& path) {
#if defined(__linux__)
    const auto resolved = fs::canonical(path);
    if (resolved.parent_path() != fs::canonical("/bin") && resolved.parent_path() != fs::canonical("/usr/bin"))
        throw Conflict("Executable resolves outside system bin");
#endif
    return path;
}

fs::path executable(const fs::path& root, const std::string& name) {
    const fs::path requested(name);
    if (requested.is_absolute()) {
        if (requested.parent_path() != "/bin" && requested.parent_path() != "/usr/bin")
            throw Conflict("Executable must be in the workspace or system bin");
        return systemExecutable(requested);
    }
    if (!requested.has_parent_path()) {
        for (const auto* prefix : {"/usr/bin","/bin"}) {
            const auto candidate = fs::path(prefix) / requested;
            if (access(candidate.c_str(),X_OK) == 0) return systemExecutable(candidate);
        }
    }
    return root / relativeToolPath(name);
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

std::vector<std::string> sandboxCommand(const fs::path& root, const fs::path& cwd,
                                        std::vector<std::string> command, bool writable,
                                        const fs::path& database, bool pipelineStage, bool includeSupervisor) {
    const auto state = stateDirectory(database);
    const auto actualDatabase = fs::weakly_canonical(database);
    for (const auto* directory : systemRoots) {
        if (!fs::exists(directory)) continue;
        const auto system = fs::canonical(directory);
        if (system == state || pathContains(system,state) || pathContains(system,root) || pathContains(system,actualDatabase))
            throw Conflict("Flock state and Bot workspaces must be outside sandbox system directories");
    }
    command.front() = executable(root,command.front()).string();
#if defined(__APPLE__)
    const auto workspace = "(subpath " + sandboxLiteral(root.string()) + ")";
    auto profile = std::string(R"PROFILE((version 1)
(deny default)
(deny process-fork)
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
    if (pathContains(root,command.front())) command.front() = inner(root,command.front());
    std::vector<std::string> launch{"/usr/bin/bwrap","--unshare-all","--unshare-user","--die-with-parent",
        "--new-session","--disable-userns","--cap-drop","ALL","--dev","/dev"};
    if (pipelineStage) launch.insert(launch.begin()+1,{"--seccomp","4"});
    for (const auto* path : systemRoots) if (fs::exists(path))
        launch.insert(launch.end(),{"--ro-bind",path,path});
    launch.insert(launch.end(),{writable ? "--bind-fd" : "--ro-bind-fd","3","/workspace",
                               "--chdir",inner(root,cwd),"--"});
#else
    (void)cwd;
    (void)writable;
    throw Conflict("No supported command sandbox on this host");
    std::vector<std::string> launch;
#endif
    if (includeSupervisor) launch.insert(launch.begin(),FLOCK_WORKSPACE_SUPERVISOR);
    launch.insert(launch.end(),command.begin(),command.end());
    return launch;
}

std::vector<std::string> sandboxCommand(const fs::path& root, const fs::path& cwd, const Json& args, bool writable,
                                        const fs::path& database) {
    return sandboxCommand(root,cwd,commandArguments(args.at("argv")),writable,database,false,true);
}

std::vector<std::string> pipelineSupervisorCommand(const fs::path& root, const fs::path& cwd, const Json& pipeline,
                                                   bool writable, const fs::path& database) {
    auto stages = pipelineArguments(pipeline);
    std::vector<std::string> command{FLOCK_WORKSPACE_SUPERVISOR,"--flock-pipeline-v1",std::to_string(stages.size())};
    for (auto& stage : stages) {
        auto sandbox = sandboxCommand(root,cwd,std::move(stage),writable,database,true,false);
        command.push_back(std::to_string(sandbox.size()));
        command.insert(command.end(),sandbox.begin(),sandbox.end());
    }
    std::size_t bytes = 0;
    for (const auto& argument : command) {
        if (argument.size() + 1 > maxPipelineBytes - std::min(bytes,maxPipelineBytes))
            throw Conflict("Pipeline arguments exceed 32768 serialized bytes; use a T3 Session");
        bytes += argument.size()+1;
    }
    return command;
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

pid_t launchSandbox(const fs::path& cwd, int directory, std::vector<std::string> command, int lease, int out, int err) {
    Spawn spawn;
    Spawn::check(posix_spawn_file_actions_adddup2(&spawn.actions,lease,STDIN_FILENO));
    Spawn::check(posix_spawn_file_actions_adddup2(&spawn.actions,out,STDOUT_FILENO));
    Spawn::check(posix_spawn_file_actions_adddup2(&spawn.actions,err,STDERR_FILENO));
    Spawn::check(posix_spawn_file_actions_adddup2(&spawn.actions,directory,3));
#ifdef __APPLE__
    Spawn::check(posix_spawnattr_setflags(&spawn.attributes,POSIX_SPAWN_CLOEXEC_DEFAULT));
    auto home = "HOME=" + cwd.string();
#else
    (void)cwd;
    Spawn::check(posix_spawn_file_actions_addclosefrom_np(&spawn.actions,4));
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

WorkspaceOutput redactOutput(const WorkspaceOutput& capture, const SecretRedactor& scrub, bool partial) {
    const auto stdoutText = safeOutput(scrub.text(capture.text[0],partial || capture.truncated));
    const auto stderrText = safeOutput(scrub.text(capture.text[1],partial || capture.truncated));
    const auto out = bounded(stdoutText,workspaceOutputLimit);
    const auto err = bounded(stderrText,workspaceOutputLimit-out.size());
    return {{out,err},capture.truncated || out != stdoutText || err != stderrText};
}

void verifyTree(int root) {
    std::size_t count = 0;
    walk(root,{},[&](int, const fs::path& path, const struct stat& info) {
        if (++count > maxEntries || path.native().size() > 1024)
            throw Conflict("Workspace tree is too large for command execution");
        if (!S_ISDIR(info.st_mode) && (!S_ISREG(info.st_mode) || info.st_nlink != 1))
            throw Conflict("Command workspace contains an unsafe entry");
        return true;
    });
}

}

bool WorkspaceOutput::readAvailable(int fd, unsigned channel) {
    char buffer[4096];
    bool changed = false;
    for (int i = 0; i < 8; ++i) {
        const auto n = read(fd,buffer,sizeof(buffer));
        if (n <= 0) break;
        const auto room = workspaceOutputLimit-text[0].size()-text[1].size();
        const auto used = std::min(static_cast<std::size_t>(n),room);
        changed |= used > 0 || !truncated;
        text[channel].append(buffer,used);
        truncated |= used < static_cast<std::size_t>(n);
    }
    return changed;
}

int openExactChildDirectory(int parent, const fs::path& name) {
    auto child = directoryAt(parent,name);
    struct stat info{};
    if (fstat(child.get(),&info) != 0) throw Conflict("Cannot identify Bot workspace");
    const bool missing = visitDirectory(parent,[&](std::string_view entry) {
        if (entry != name.native()) return true;
        struct stat named{};
        return fstatat(parent,name.c_str(),&named,AT_SYMLINK_NOFOLLOW) != 0 ||
               named.st_dev != info.st_dev || named.st_ino != info.st_ino;
    });
    if (missing) throw Conflict("Bot workspace does not have the exact directory name");
    return child.release();
}

fs::path relativeToolPath(std::string_view value) {
    fs::path path(value);
    if (value.empty() || value.size() > 1024 || value.find('\0') != value.npos || path.is_absolute() ||
        path.filename().empty() || std::ranges::any_of(path,[](const auto& part) { return part == ".."; }))
        throw Conflict("Expected a relative path inside the authorized directory");
    return path.lexically_normal();
}

bool pathContains(const fs::path& root, const fs::path& child) {
    return std::mismatch(root.begin(),root.end(),child.begin(),child.end()).first == root.end();
}

Json readToolFile(int root, const fs::path& relative, const SecretRedactor& scrub) {
    const auto parent = directoryAt(root,relative.parent_path());
    const auto file = regularFile(parent.get(),relative.filename());
    const auto data = readFile(file.get(),maxFileBytes+1);
    const auto redacted = safeOutput(scrub.text(std::string_view(data).substr(0,maxFileBytes),data.size() > maxFileBytes));
    return {{"text",bounded(redacted,maxFileBytes)},{"truncated",data.size() > maxFileBytes || redacted.size() > maxFileBytes}};
}

Json readToolFile(const fs::path& root, const fs::path& relative, const SecretRedactor& scrub) {
    const auto fd = directory(root);
    return readToolFile(fd.get(),relative,scrub);
}

void Store::Impl::initializeWorkspaceSchema() {
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
    persistWorkspaceResults();
    workspaceJobs.clear();
    exec("UPDATE workspace_commands SET state='unknown_after_restart' WHERE state IN ('running','stop_requested')");
}

void Store::Impl::persistWorkspaceResults() {
    std::lock_guard lock(mutex);
    for (auto it = workspaceJobs.begin(); it != workspaceJobs.end();) {
        const auto& completion = it->second->completion;
        if (const auto* error = std::get_if<std::exception_ptr>(&completion)) std::rethrow_exception(*error);
        if (const auto* outcome = std::get_if<WorkspaceOutcome>(&completion)) {
            const auto output = redactOutput(outcome->capture,sharedRedactor(),outcome->incomplete);
            exec("UPDATE workspace_commands SET stdout=?,stderr=?,truncated=?,state=?,exit_code=? WHERE id=?",
                 {output.text[0],output.text[1],int(output.truncated),
                  outcome->exitCode ? "exited" : "unknown_after_restart",
                  outcome->exitCode ? Json(*outcome->exitCode) : Json(nullptr),it->first});
            it = workspaceJobs.erase(it);
        } else ++it;
    }
}

void Store::Impl::stopWorkspace() {
    {
        std::lock_guard lock(mutex);
        for (auto& [id,job] : workspaceJobs) job->worker.request_stop();
    }
    // Runtime has joined the workers that can admit commands. Only command workers remain.
    for (auto& [id,job] : workspaceJobs)
        if (job->worker.joinable()) job->worker.join();
    for (int attempt = 1; ; ++attempt) {
        try {
            persistWorkspaceResults();
            break;
        } catch (const StorageError& error) {
            if (!error.contended() || attempt == 3) throw;
            std::this_thread::sleep_for(50ms);
        }
    }
    workspaceJobs.clear();
}

void Store::Impl::validateWorkspace(const ToolContext& ctx, const Decision& call) {
    const auto& args = call.args;
    if (call.tool == "workspace_command_status" || call.tool == "workspace_stop_command") {
        if (!query("SELECT 1 FROM workspace_commands WHERE bot=? AND id=?",
                   {ctx.bot,toolId(args,"commandId")}).next()) throw Conflict("Unknown workspace command");
        return;
    }
    if (call.tool == "workspace_run_command") {
        if (args.contains("cwd")) relativeToolPath(toolText(args,"cwd",1024));
        if (args.contains("argv") == args.contains("pipeline"))
            throw Conflict("Use exactly one of argv or pipeline");
        if (args.contains("pipeline")) {
#if defined(__APPLE__) || defined(__linux__)
            pipelineArguments(args.at("pipeline"));
#else
            throw Conflict("Workspace pipelines are unavailable on this host; use a T3 Session");
#endif
        } else {
            executable({},commandArguments(args.at("argv")).front());
        }
        return;
    }
    if (call.tool != "workspace_search" || args.contains("path")) {
        const auto relative = relativeToolPath(toolText(args,"path",1024));
        if (relative == "." && call.tool != "workspace_search") throw Conflict("Expected a workspace file");
    }
}

Json Store::Impl::workspaceTool(const ToolContext& ctx, const Decision& call) {
    const auto& args = call.args;
    if (call.tool == "workspace_command_status" || call.tool == "workspace_stop_command") {
        std::lock_guard lock(mutex);
        persistWorkspaceResults();
        const auto id = toolId(args,"commandId");
        if (call.tool == "workspace_stop_command") {
            exec("UPDATE workspace_commands SET state='stop_requested' WHERE bot=? AND id=? AND state='running'",{ctx.bot,id});
            if (sqlite3_changes(db)) {
                const auto job = workspaceJobs.find(id);
                if (job != workspaceJobs.end()) job->second->stop = true;
            }
            auto row = query("SELECT state FROM workspace_commands WHERE bot=? AND id=?",{ctx.bot,id});
            if (!row.next()) throw Conflict("Unknown workspace command");
            return {{"commandId",id},{"state",row.string(0)}};
        }
        auto row = query("SELECT state,stdout,stderr,truncated,coalesce(exit_code,-1) "
                         "FROM workspace_commands WHERE bot=? AND id=?",{ctx.bot,id});
        if (!row.next()) throw Conflict("Unknown workspace command");
        const auto exit = row.number(4);
        return {{"commandId",id},{"state",row.string(0)},{"stdout",row.string(1)},{"stderr",row.string(2)},
                {"truncated",row.number(3)!=0},{"exitCode",exit < 0 ? Json(nullptr) : Json(exit)}};
    }
    auto workspace = openWorkspace(path,ctx);
    const auto root = workspace.directory.get();
    if (call.tool == "workspace_search") {
        auto relative = args.contains("path") ? relativeToolPath(toolText(args,"path",1024)) : fs::path{};
        if (relative == ".") relative.clear();
        return searchWorkspace(root,relative,toolText(args,"query",256));
    }
    if (call.tool == "workspace_read_file") return readToolFile(root,relativeToolPath(toolText(args,"path",1024)),sharedRedactor());
    if (call.tool == "workspace_write_file") {
        const auto data = toolText(args,"text",maxFileBytes,true);
        writeFile(root,relativeToolPath(toolText(args,"path",1024)),data);
        return {{"bytes",data.size()}};
    }

    auto relative = args.contains("cwd") ? relativeToolPath(toolText(args,"cwd",1024)) : fs::path{};
    if (relative == ".") relative.clear();
    auto working = directoryAt(root,relative);
    verifyTree(root);
    const auto cwd = relative.empty() ? workspace.path : workspace.path / relative;
    const bool writable = Access(*this).permits(ctx.bot,{"workspace",ctx.bot,"write"},true);
    const bool pipeline = args.contains("pipeline");
#if defined(__APPLE__) || defined(__linux__)
    auto command = pipeline ? pipelineSupervisorCommand(workspace.path,cwd,args.at("pipeline"),writable,path)
                            : sandboxCommand(workspace.path,cwd,args,writable,path);
#else
    if (pipeline) throw Conflict("Workspace pipelines are unavailable on this host; use a T3 Session");
    auto command = sandboxCommand(workspace.path,cwd,args,writable,path);
#endif
    Pipe out, err, lease;
    std::lock_guard lock(mutex);
    persistWorkspaceResults();
    if (workspaceJobs.size() >= 8 || query("SELECT 1 FROM workspace_commands WHERE bot=? AND state IN "
                                          "('running','stop_requested')",{ctx.bot}).next())
        throw Conflict("Workspace command limit reached");
    auto job = std::make_unique<WorkspaceJob>();
    auto* control = job.get();
    exec("INSERT INTO workspace_commands(bot,started_at,state) VALUES(?,?,'running')",{ctx.bot,unixTime()});
    const auto id = last();
    workspaceJobs.emplace(id,std::move(job));
    try {
        control->worker = std::jthread([this,control,id,cwd,pipeline,command = std::move(command),
#ifdef __APPLE__
                                        working = std::move(working),
#else
                                        working = std::move(workspace.directory),
#endif
                                        out = std::move(out),err = std::move(err),lease = std::move(lease)](std::stop_token shutdown) mutable {
            pid_t pid = 0;
            bool supervisorReaped = false;
            bool workerTimeout = false;
            int supervisorStatus = 0;
            WorkspaceOutput capture;
            try {
                if (control->stop || shutdown.stop_requested()) lease.ends.second.reset();
                pid = launchSandbox(cwd,working.get(),std::move(command),lease.ends.first.get(),out.ends.second.get(),err.ends.second.get());
                working.reset();
                lease.ends.first.reset();
                out.ends.second.reset();
                err.ends.second.reset();
                auto readers = std::array{pollfd{out.ends.first.get(),POLLIN,0},pollfd{err.ends.first.get(),POLLIN,0}};
                const auto deadline = std::chrono::steady_clock::now() + supervisorDeadline;
                while (true) {
                    if (control->stop || shutdown.stop_requested()) lease.ends.second.reset();
                    if (std::chrono::steady_clock::now() >= deadline) {
                        workerTimeout = true;
                        throw Conflict("Sandbox supervisor exceeded its deadline");
                    }
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
                        supervisorReaped = true;
                        supervisorStatus = status;
                        for (unsigned i = 0; i < readers.size(); ++i) capture.readAvailable(readers[i].fd,i);
                        if (shutdown.stop_requested()) return;
                        std::lock_guard guard(mutex);
                        const bool unknown = pipeline && WIFSIGNALED(status);
                        const auto exit = WIFEXITED(status) ? WEXITSTATUS(status) :
                                          WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
                        control->completion = WorkspaceOutcome{std::move(capture),unknown ? std::nullopt : std::optional{exit}};
                        return;
                    }
                    if (changed) {
                        try {
                            std::lock_guard guard(mutex);
                            const auto partial = redactOutput(capture,sharedRedactor(),true);
                            exec("UPDATE workspace_commands SET stdout=?,stderr=?,truncated=? WHERE id=?",
                                 {partial.text[0],partial.text[1],int(partial.truncated),id});
                        } catch (const StorageError& error) {
                            if (!error.contended()) throw;
                        }
                    }
                }
            } catch (...) {
                const auto error = std::current_exception();
                lease.ends.second.reset();
                if (pid > 0) {
                    int status = 0;
                    pid_t waited;
                    do waited = waitpid(pid,&status,0); while (waited < 0 && errno == EINTR);
                    if (waited == pid) {
                        supervisorReaped = true;
                        supervisorStatus = status;
                    }
                }
                std::lock_guard guard(mutex);
                try { std::rethrow_exception(error); }
                catch (const Conflict&) {
                    if (shutdown.stop_requested()) return;
                    const bool unknown = pipeline &&
                                         ((supervisorReaped && WIFSIGNALED(supervisorStatus)) ||
                                          (!supervisorReaped && pid > 0));
                    const int exit = workerTimeout ? 124 : 125;
                    capture.text[1] = "Sandbox command or supervisor failed";
                    control->completion = WorkspaceOutcome{std::move(capture),
                        unknown ? std::nullopt : std::optional{exit},true};
                } catch (...) { control->completion = error; }
            }
        });
    } catch (const std::system_error&) {
        control->completion = WorkspaceOutcome{{},125};
        throw Conflict("Could not start the workspace command");
    }
    return {{"commandId",id},{"state","running"}};
}
}
