#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef __linux__
#include <seccomp.h>
#include <sys/prctl.h>
#endif

namespace {
using namespace std::chrono_literals;

struct Fd {
    int value = -1;
    Fd() = default;
    explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(std::exchange(other.value,-1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            value = std::exchange(other.value,-1);
        }
        return *this;
    }
    ~Fd() { reset(); }
    int get() const { return value; }
    void reset() {
        if (value >= 0) close(std::exchange(value,-1));
    }
};

struct Pipe {
    Fd read;
    Fd write;
};

struct Stage {
    std::vector<std::string> args;
    std::vector<char*> argv;
    pid_t pid = -1;
    bool reaped = false;
    int status = 0;
};

#ifdef __linux__
struct Filter {
    Fd read;
    Fd write;
};
#endif

bool setLimit(int resource, rlim_t value) {
    const rlimit limit{value,value};
    return setrlimit(resource,&limit) == 0;
}

bool leaseEnded(int timeout) {
    pollfd lease{STDIN_FILENO,POLLIN,0};
    const auto ready = poll(&lease,1,timeout);
    return (ready < 0 && errno != EINTR) || (ready > 0 && lease.revents);
}

bool parseCount(const char* value, std::size_t maximum, std::size_t& result) {
    if (!value || !*value) return false;
    const auto end = value + std::char_traits<char>::length(value);
    const auto parsed = std::from_chars(value,end,result);
    return parsed.ec == std::errc{} && parsed.ptr == end && result <= maximum;
}

bool parseStages(int argc, char** argv, std::vector<Stage>& stages) {
    if (argc < 3 || std::string_view(argv[1]) != "--flock-pipeline-v1") return false;
    std::size_t count = 0;
    if (!parseCount(argv[2],4,count) || count == 0) return false;
    int index = 3;
    std::size_t bytes = 0;
    stages.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (index >= argc) return false;
        std::size_t argcStage = 0;
        if (!parseCount(argv[index++],128,argcStage) || argcStage == 0 ||
            static_cast<std::size_t>(argc-index) < argcStage) return false;
        Stage stage;
        stage.args.reserve(argcStage);
        for (std::size_t j = 0; j < argcStage; ++j) {
            const std::string value(argv[index++]);
            if (value.size()+1 > 32768-std::min(bytes,std::size_t{32768})) return false;
            bytes += value.size()+1;
            stage.args.push_back(value);
        }
        if (stage.args.front().empty()) return false;
        for (auto& value : stage.args) stage.argv.push_back(value.data());
        stage.argv.push_back(nullptr);
        stages.push_back(std::move(stage));
    }
    return index == argc;
}

bool makePipe(Pipe& pipe) {
    int fds[2];
#ifdef __linux__
    if (::pipe2(fds,O_CLOEXEC) != 0) return false;
#else
    if (::pipe(fds) != 0) return false;
    if (fcntl(fds[0],F_SETFD,FD_CLOEXEC) < 0 || fcntl(fds[1],F_SETFD,FD_CLOEXEC) < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
#endif
    pipe.read = Fd(fds[0]);
    pipe.write = Fd(fds[1]);
    return true;
}

#ifdef __linux__
bool createFilter(Filter& filter) {
    int fds[2];
    if (pipe2(fds,O_CLOEXEC) != 0) return false;
    filter.read = Fd(fds[0]);
    filter.write = Fd(fds[1]);
    auto context = seccomp_init(SCMP_ACT_ALLOW);
    if (!context) return false;
    bool ok = seccomp_attr_set(context,SCMP_FLTATR_ACT_BADARCH,SCMP_ACT_KILL_PROCESS) == 0;
    constexpr std::array<const char*,4> syscalls{"fork","vfork","clone","clone3"};
    for (const auto* name : syscalls) {
        const int number = seccomp_syscall_resolve_name(name);
        if (number < 0) {
            if (std::string_view(name) == "fork" || std::string_view(name) == "vfork") continue;
            ok = false;
            break;
        }
        if (seccomp_rule_add(context,SCMP_ACT_ERRNO(EPERM),number,0) != 0) {
            ok = false;
            break;
        }
    }
    if (ok) ok = seccomp_export_bpf(context,filter.write.get()) == 0;
    seccomp_release(context);
    filter.write.reset();
    return ok;
}

bool childPids(std::vector<pid_t>& pids) {
    const auto path = "/proc/self/task/" + std::to_string(getpid()) + "/children";
    Fd file(open(path.c_str(),O_RDONLY|O_CLOEXEC));
    if (file.get() < 0) return false;
    std::string data;
    char buffer[512];
    for (;;) {
        const auto count = read(file.get(),buffer,sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return false;
        if (count == 0) break;
        data.append(buffer,static_cast<std::size_t>(count));
        if (data.size() > 8192) return false;
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        while (offset < data.size() && (data[offset] == ' ' || data[offset] == '\n' || data[offset] == '\t'))
            ++offset;
        if (offset == data.size()) break;
        pid_t pid = 0;
        const auto parsed = std::from_chars(data.data()+offset,data.data()+data.size(),pid);
        if (parsed.ec != std::errc{} || parsed.ptr == data.data()+offset || pid <= 0) return false;
        pids.push_back(pid);
        offset = static_cast<std::size_t>(parsed.ptr-data.data());
        if (offset < data.size() && data[offset] != ' ' && data[offset] != '\n' && data[offset] != '\t') return false;
    }
    return true;
}

bool prepareSubreaper() {
    if (prctl(PR_SET_CHILD_SUBREAPER,1) != 0) return false;
    std::vector<pid_t> children;
    return childPids(children) && children.empty();
}
#endif

void rememberReaped(std::vector<Stage>& stages, pid_t pid, int status, std::size_t& live) {
    const auto found = std::ranges::find_if(stages,[&](const Stage& stage) { return stage.pid == pid; });
    if (found != stages.end() && !found->reaped) {
        found->reaped = true;
        found->status = status;
        if (live) --live;
    }
}

void reapAndKill(std::vector<Stage>& stages) {
    for (const auto& stage : stages)
        if (stage.pid > 0 && !stage.reaped) kill(stage.pid,SIGKILL);
    for (;;) {
#ifdef __linux__
        std::vector<pid_t> children;
        if (childPids(children))
            for (const auto pid : children) kill(pid,SIGKILL);
#endif
        for (;;) {
            int status = 0;
            const auto child = waitpid(-1,&status,WNOHANG);
            if (child > 0) {
                std::size_t ignored = 0;
                rememberReaped(stages,child,status,ignored);
                continue;
            }
            if (child < 0 && errno == EINTR) continue;
            if (child < 0 && errno == ECHILD) return;
            break;
        }
        poll(nullptr,0,10);
    }
}

void execStage(std::size_t index, std::vector<Stage>& stages, std::vector<Pipe>& pipes, int nullFd
#ifdef __linux__
               , std::vector<Filter>& filters
#endif
) {
    const int input = index == 0 ? nullFd : pipes[index-1].read.get();
    const int output = index+1 == stages.size() ? STDOUT_FILENO : pipes[index].write.get();
    if (dup2(input,STDIN_FILENO) < 0 || dup2(output,STDOUT_FILENO) < 0) _exit(125);
    for (const auto& pipe : pipes) {
        close(pipe.read.get());
        close(pipe.write.get());
    }
    if (nullFd >= 0) close(nullFd);
#ifdef __linux__
    const int filter = filters[index].read.get();
    for (std::size_t i = 0; i < filters.size(); ++i)
        if (i != index) close(filters[i].read.get());
    if (filter != 4) {
        if (dup2(filter,4) < 0) _exit(125);
        close(filter);
    } else {
        const int flags = fcntl(4,F_GETFD);
        if (flags < 0 || fcntl(4,F_SETFD,flags & ~FD_CLOEXEC) < 0) _exit(125);
    }
#endif
    const auto cpu = std::max<rlim_t>(1,5/stages.size());
    if (!setLimit(RLIMIT_CORE,0) || !setLimit(RLIMIT_FSIZE,1024*1024) ||
        !setLimit(RLIMIT_CPU,cpu) || !setLimit(RLIMIT_NOFILE,64)) _exit(125);
    execv(stages[index].args.front().c_str(),stages[index].argv.data());
    _exit(125);
}

int stageStatus(const Stage& stage) {
    if (WIFEXITED(stage.status)) return WEXITSTATUS(stage.status);
    if (WIFSIGNALED(stage.status)) return 128+WTERMSIG(stage.status);
    return 125;
}

int pipelineStatus(const std::vector<Stage>& stages) {
    for (auto stage = stages.rbegin(); stage != stages.rend(); ++stage) {
        const int result = stageStatus(*stage);
        if (result != 0) return result;
    }
    return 0;
}

int runPipeline(int argc, char** argv) {
    const auto deadline = std::chrono::steady_clock::now()+10s;
    std::vector<Stage> stages;
    if (!parseStages(argc,argv,stages)) return 125;
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD,&action,nullptr) != 0) return 125;
#ifdef __linux__
    if (!prepareSubreaper()) return 125;
    std::vector<Filter> filters(stages.size());
    for (auto& filter : filters)
        if (!createFilter(filter)) return 125;
#endif
    std::vector<Pipe> pipes(stages.size()-1);
    for (auto& pipe : pipes)
        if (!makePipe(pipe)) return 125;
    const int nullFd = open("/dev/null",O_RDONLY|O_CLOEXEC);
    if (nullFd < 0) return 125;
    std::size_t live = 0;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (leaseEnded(0)) {
            reapAndKill(stages);
            close(nullFd);
            return 130;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            reapAndKill(stages);
            close(nullFd);
            return 124;
        }
        const auto child = fork();
        if (child < 0) {
            reapAndKill(stages);
            close(nullFd);
            return 125;
        }
        if (child == 0) {
            execStage(i,stages,pipes,nullFd
#ifdef __linux__
                      ,filters
#endif
            );
        }
        stages[i].pid = child;
        ++live;
    }
    for (auto& pipe : pipes) {
        pipe.read.reset();
        pipe.write.reset();
    }
    close(nullFd);
#ifdef __linux__
    for (auto& filter : filters) filter.read.reset();
#endif
    for (;;) {
        if (leaseEnded(0)) {
            reapAndKill(stages);
            return 130;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            reapAndKill(stages);
            return 124;
        }
        bool noChildren = false;
        for (;;) {
            int status = 0;
            const auto child = waitpid(-1,&status,WNOHANG);
            if (child > 0) {
                rememberReaped(stages,child,status,live);
                continue;
            }
            if (child < 0 && errno == EINTR) continue;
            if (child < 0 && errno == ECHILD) noChildren = true;
            else if (child < 0) {
                reapAndKill(stages);
                return 125;
            }
            break;
        }
        if (noChildren) return live == 0 ? pipelineStatus(stages) : 125;
        if (live == 0) {
            reapAndKill(stages);
            return 125;
        }
        if (leaseEnded(25)) {
            reapAndKill(stages);
            return 130;
        }
    }
}
}

int main(int argc, char** argv) {
    if (argc < 2) return 125;
#ifdef __APPLE__
    if (fchdir(3) != 0 || close(3) != 0) return 125;
#endif
    if (std::string_view(argv[1]) == "--flock-pipeline-v1") return runPipeline(argc,argv);
    if (leaseEnded(0)) return 130;
    const auto child = fork();
    if (child < 0) return 125;
    if (child == 0) {
#ifdef __APPLE__
        if (setpgid(0,0) != 0) _exit(125);
#endif
        const int null = open("/dev/null",O_RDONLY | O_CLOEXEC);
        if (null < 0 || dup2(null,STDIN_FILENO) < 0 || !setLimit(RLIMIT_CORE,0) ||
            !setLimit(RLIMIT_FSIZE,1024*1024) || !setLimit(RLIMIT_CPU,5) || !setLimit(RLIMIT_NOFILE,64))
            _exit(125);
        close(null);
        execv(argv[1],argv+1);
        _exit(125);
    }
#ifndef __APPLE__
    close(3);
#endif
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        int status = 0;
        const auto done = waitpid(child,&status,WNOHANG);
        if (done > 0) return WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status);
        if (done < 0 && errno != EINTR) return 125;
        const bool timeout = std::chrono::steady_clock::now() >= deadline;
        if (timeout || leaseEnded(25)) {
#ifdef __APPLE__
            if (kill(-child,SIGKILL) != 0) kill(child,SIGKILL);
#else
            kill(child,SIGKILL);
#endif
            while (waitpid(child,nullptr,0) < 0 && errno == EINTR) {}
            return timeout ? 124 : 130;
        }
    }
}
