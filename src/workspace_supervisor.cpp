#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;

bool setLimit(int resource, rlim_t value) {
    const rlimit limit{value,value};
    return setrlimit(resource,&limit) == 0;
}

bool leaseEnded(int timeout) {
    pollfd lease{STDIN_FILENO,POLLIN,0};
    const auto ready = poll(&lease,1,timeout);
    return (ready < 0 && errno != EINTR) || (ready > 0 && lease.revents);
}
}

int main(int argc, char** argv) {
    if (argc < 2) return 125;
    if (leaseEnded(0)) return 130;
    const auto child = fork();
    if (child < 0) return 125;
    if (child == 0) {
        const int null = open("/dev/null",O_RDONLY | O_CLOEXEC);
        if (null < 0 || dup2(null,STDIN_FILENO) < 0 || !setLimit(RLIMIT_CORE,0) ||
            !setLimit(RLIMIT_FSIZE,1024*1024) || !setLimit(RLIMIT_CPU,5) || !setLimit(RLIMIT_NOFILE,64))
            _exit(125);
        close(null);
        execv(argv[1],argv+1);
        _exit(125);
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        int status = 0;
        const auto done = waitpid(child,&status,WNOHANG);
        if (done > 0) return WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status);
        if (done < 0 && errno != EINTR) return 125;
        const bool timeout = std::chrono::steady_clock::now() >= deadline;
        if (timeout || leaseEnded(25)) {
            kill(child,SIGKILL);
            while (waitpid(child,nullptr,0) < 0 && errno == EINTR) {}
            return timeout ? 124 : 130;
        }
    }
}
