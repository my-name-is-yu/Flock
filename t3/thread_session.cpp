#include "internal.hpp"
#include "flock/root_delivery.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace flockv0::t3detail {
namespace {

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { close(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
private:
    int fd_;
};

class SpawnActions {
public:
    SpawnActions() : ready(posix_spawn_file_actions_init(&actions) == 0) {}
    ~SpawnActions() { if (ready) posix_spawn_file_actions_destroy(&actions); }
    posix_spawn_file_actions_t actions{};
    const bool ready;
};

std::optional<std::string> gitOutput(const std::filesystem::path& directory,
                                     std::initializer_list<std::string> arguments,
                                     bool allowMultipleLines = false) {
    int pipes[2];
#if defined(__linux__)
    if (pipe2(pipes, O_CLOEXEC) != 0) return {};
#else
    if (pipe(pipes) != 0) return {};
#endif
    Fd reader(pipes[0]), writer(pipes[1]);
#if !defined(__linux__)
    if (fcntl(reader.get(), F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(writer.get(), F_SETFD, FD_CLOEXEC) != 0) return {};
#endif
    SpawnActions spawn;
    if (!spawn.ready ||
        posix_spawn_file_actions_addclose(&spawn.actions, reader.get()) != 0 ||
        posix_spawn_file_actions_adddup2(&spawn.actions, writer.get(), STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_addclose(&spawn.actions, writer.get()) != 0 ||
        posix_spawn_file_actions_addopen(&spawn.actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&spawn.actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) != 0)
        return {};
#if defined(__linux__)
    if (posix_spawn_file_actions_addclosefrom_np(&spawn.actions, 3) != 0) return {};
#endif
    std::vector<std::string> words{"/usr/bin/git", "-C", directory.string()};
    words.insert(words.end(), arguments);
    std::vector<char*> argv;
    for (auto& word : words) argv.push_back(word.data());
    argv.push_back(nullptr);
    std::string path = "PATH=/usr/bin:/bin";
    std::string noSystem = "GIT_CONFIG_NOSYSTEM=1", noGlobal = "GIT_CONFIG_GLOBAL=/dev/null";
    std::string noLazyFetch = "GIT_NO_LAZY_FETCH=1", noReplace = "GIT_NO_REPLACE_OBJECTS=1";
    std::string noOptionalLocks = "GIT_OPTIONAL_LOCKS=0";
    char* env[]{path.data(), noSystem.data(), noGlobal.data(), noLazyFetch.data(), noReplace.data(),
                noOptionalLocks.data(), nullptr};
    pid_t child = 0;
#if defined(__APPLE__)
    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0) return {};
    const auto flagsError = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    const auto spawnError = flagsError == 0
        ? posix_spawn(&child, argv.front(), &spawn.actions, &attributes, argv.data(), env) : flagsError;
    posix_spawnattr_destroy(&attributes);
#else
    const auto spawnError = posix_spawn(&child, argv.front(), &spawn.actions, nullptr, argv.data(), env);
#endif
    if (spawnError != 0) return {};
    writer.close();
    std::string output;
    bool completed = false;
    const auto expires = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < expires && output.size() <= 4096) {
        pollfd descriptor{reader.get(), POLLIN, 0};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            expires - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;
        const int ready = poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        std::array<char, 1024> buffer;
        const auto count = ::read(reader.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            completed = count == 0;
            break;
        }
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!completed) kill(child, SIGKILL);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (!completed || waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        (!allowMultipleLines && output.empty()) || (!output.empty() && output.back() != '\n'))
        return {};
    if (!output.empty()) output.pop_back();
    if (output.find('\r') != std::string::npos || output.find('\0') != std::string::npos ||
        (!allowMultipleLines && output.find('\n') != std::string::npos)) return {};
    return output;
}

std::optional<std::filesystem::path> localDirectory(std::string_view candidate) {
    if (candidate.find('\0') != candidate.npos) return {};
    const std::filesystem::path path(candidate);
    if (!path.is_absolute()) return {};
    std::error_code error;
    auto canonical = std::filesystem::canonical(path, error);
    if (error || !std::filesystem::is_directory(canonical, error) || error) return {};
    return canonical;
}

bool completeMessage(std::string_view status) {
    return status == "completed" || status == "failed" || status == "cancelled" || status == "interrupted";
}

bool validDestination(const Json& thread, const ThreadTarget& target) {
    return str(thread,"projectId") == target.projectId && !boolean(thread,"archived");
}

void verifyEnvironment(Client& client, const ThreadTarget& target) {
    const auto environment = client.call("t3_environment_read", Json::object());
    if (str(environment,"environmentId") != target.environmentId)
        throw Failure(Failure::Kind::Settings, "The OAuth endpoint belongs to a different T3 environment");
}

class StockThreadSession final : public ThreadSession {
public:
    explicit StockThreadSession(std::filesystem::path directory) : directory_(std::move(directory)) {}

    std::optional<ThreadPage> read(const ThreadTarget& target, std::optional<Id> after) override {
        try {
            auto credential = loadCredential(directory_,target.environmentId);
            if (!credential.operate) return {};
            const auto generation = credential.generation;
            Client client(std::move(credential));
            verifyEnvironment(client,target);
            const auto bounds = after ? Json{{"afterPosition",*after}} : Json{{"beforePosition",maxHistoryPosition}};
            const auto reply = readThread(client,target.threadId,"messages",20,2730,bounds);
            const auto& thread = member(reply,"thread");
            if (!validDestination(thread,target)) return {};
            const auto& running = member(thread,"activeRunId");
            if (!running.is_null() && (!running.is_string() || str(thread,"activeRunId").empty())) invalidResponse();
            ThreadPage page{target,after.value_or(-1),generation,str(thread,"activeRunId"),{}};
            auto rows = array(reply,"items").get<Json::array_t>();
            if (!after)
                std::ranges::sort(rows,{},[](const auto& item) { return position(member(item,"position")); });
            for (const auto& item : rows) {
                const auto current = position(member(item,"position"));
                if (current <= page.cursor) invalidResponse();
                const auto kind = str(item,"type");
                const auto status = str(item,"status");
                const auto local = localItem(item,target.threadId);
                // Streaming assistant messages retain their position when finalized.
                if (local && kind == "assistant_message" && !completeMessage(status)) break;
                page.cursor = current;
                if (!local || (kind != "user_message" && kind != "assistant_message")) continue;
                const auto id = str(item,"itemId");
                if (id.empty() || id.size() > 1024) invalidResponse();
                page.events.push_back({id,str(item,"messageId"),str(item,"runId"),kind,status,
                    str(item,"createdBy"),str(item,"text"),current,str(item,"creationSource")});
            }
            return page;
        } catch (const Failure&) {
            return {};
        }
    }

    std::optional<ThreadPage> dispatchHistory(const ThreadTarget& target, Id after) override {
        constexpr unsigned pageSize = 20;
        constexpr unsigned maxPages = 32;
        constexpr unsigned maxItems = pageSize * maxPages;
        if (after < -1 || after > maxHistoryPosition) return {};
        ThreadPage history{target,after,"","",{}};
        unsigned scanned = 0;
        try {
            for (unsigned pageIndex = 0; pageIndex < maxPages; ++pageIndex) {
                auto credential = loadCredential(directory_,target.environmentId);
                if (!credential.operate || (!history.authGeneration.empty() &&
                    credential.generation != history.authGeneration)) return {};
                if (history.authGeneration.empty()) history.authGeneration = credential.generation;
                Client client(std::move(credential));
                verifyEnvironment(client,target);
                const auto start = history.cursor;
                const auto reply = readThread(client,target.threadId,"messages",pageSize,2730,
                                              {{"afterPosition",start}});
                const auto& thread = member(reply,"thread");
                if (!validDestination(thread,target)) return {};
                const auto& running = member(thread,"activeRunId");
                if (!running.is_null() && (!running.is_string() || str(thread,"activeRunId").empty()))
                    invalidResponse();
                history.activeRunId = str(thread,"activeRunId");
                const auto& rows = array(reply,"items");
                if (scanned + rows.size() > maxItems) return {};
                scanned += static_cast<unsigned>(rows.size());
                for (const auto& item : rows) {
                    const auto current = position(member(item,"position"));
                    if (current <= history.cursor) invalidResponse();
                    history.cursor = current;
                    const auto kind = str(item,"type");
                    if (!localItem(item,target.threadId) ||
                        (kind != "user_message" && kind != "assistant_message")) continue;
                    const auto id = str(item,"itemId");
                    if (id.empty() || id.size() > 1024) invalidResponse();
                    history.events.push_back({id,str(item,"messageId"),str(item,"runId"),kind,str(item,"status"),
                        str(item,"createdBy"),str(item,"text"),current,str(item,"creationSource")});
                }
                if (!moreHistory(reply,history.cursor,start)) return history;
                if (pageIndex + 1 == maxPages) return {};
            }
        } catch (const std::exception&) {
            return {};
        }
        return {};
    }

    ThreadSend send(const ThreadTarget& target, std::string_view generation,
                    std::string_view requestId, std::string_view text) override {
        bool dispatched = false;
        try {
            auto credential = loadCredential(directory_,target.environmentId);
            if (!credential.operate || credential.generation != generation ||
                requestId.empty() || requestId.size() > 256 || trim(text).empty() ||
                text.size() > maxSessionPromptBytes)
                return {ThreadSend::Rejected,{}};
            Client client(std::move(credential));
            verifyEnvironment(client,target);
            const auto page = readThread(client,target.threadId,"messages",1,1);
            if (!validDestination(member(page,"thread"),target)) return {ThreadSend::Rejected,{}};
            dispatched = true;
            const auto reply = client.call("t3_thread_send",{{"threadId",target.threadId},
                {"mode","queue"},{"clientRequestId",requestId},{"message",text}});
            ThreadReceipt receipt{str(reply,"messageId"),str(reply,"runId"),str(reply,"delivery")};
            if (str(reply,"threadId") != target.threadId || receipt.messageId.empty() || receipt.runId.empty() ||
                (receipt.mode != "started" && receipt.mode != "queued")) invalidResponse();
            return {ThreadSend::Accepted,std::move(receipt)};
        } catch (const Failure& error) {
            if (dispatched && error.kind != Failure::Kind::Auth && error.reason != Failure::Reason::HistoryNotFound)
                return {ThreadSend::Unknown,{}};
            return {ThreadSend::Rejected,{}};
        }
    }

    std::optional<std::string> inputCommit(const ThreadTarget& target) override {
        try {
            auto credential = loadCredential(directory_,target.environmentId);
            if (!loopbackGitHost(checkedUrl(credential.mcpUrl).origin)) return {};
            Client client(std::move(credential));
            verifyEnvironment(client,target);
            const auto page = readThread(client,target.threadId,"messages",1,1);
            const auto& thread = member(page,"thread");
            if (!validDestination(thread,target)) return {};
            const auto head = workspaceHead(client,target.projectId,thread,false);
            return head ? std::optional(head->commit) : std::nullopt;
        } catch (const Failure&) {
            return {};
        }
    }

private:
    std::filesystem::path directory_;
};

}

bool gitCommit(std::string_view sha) {
    return (sha.size() == 40 || sha.size() == 64) && std::ranges::all_of(sha,[](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool loopbackGitHost(std::string_view origin) {
    const auto authority = origin.substr(origin.find("://") + 3);
    return std::ranges::any_of(std::array<std::string_view,3>{"127.0.0.1","localhost","[::1]"},
        [&](const auto loopback) {
            return authority == loopback || (authority.starts_with(loopback) && authority[loopback.size()] == ':');
        });
}

std::optional<WorkspaceHead> workspaceHead(Client& client, std::string_view projectId, const Json& thread,
                                            bool requireSeparateWorktree) {
    const auto project = client.call("t3_project_read",{{"projectId",projectId}});
    if (str(project,"id") != projectId) return {};
    const auto root = localDirectory(str(project,"workspaceRoot"));
    if (!root) return {};
    const auto& path = member(thread,"worktreePath");
    if (!path.is_null() && !path.is_string()) return {};
    const auto workspace = path.is_null() ? root : localDirectory(path.get_ref<const std::string&>());
    if (!workspace || (requireSeparateWorktree && workspace == root)) return {};
    const auto top = gitOutput(*workspace,{"rev-parse","--show-toplevel"});
    if (!top || (workspace != root && localDirectory(*top) != workspace)) return {};
    if (workspace != root) {
        const auto rootGit = gitOutput(*root,{"rev-parse","--path-format=absolute","--git-common-dir"});
        const auto childGit = gitOutput(*workspace,{"rev-parse","--path-format=absolute","--git-common-dir"});
        if (!rootGit || !childGit) return {};
        const auto checkedRoot = localDirectory(*rootGit);
        if (!checkedRoot || checkedRoot != localDirectory(*childGit)) return {};
    }
    bool clean = true;
    if (requireSeparateWorktree) {
        const auto status = gitOutput(*workspace,{"status","--porcelain=v1","--untracked-files=no",
                                                   "--ignore-submodules=none"},true);
        if (!status) return {};
        clean = status->empty();
    }
    auto sha = gitOutput(*workspace,{"rev-parse","--verify","HEAD^{commit}"});
    return sha && gitCommit(*sha) ? std::optional(WorkspaceHead{*sha,clean}) : std::nullopt;
}

}

namespace flockv0 {

std::unique_ptr<ThreadSession> makeT3ThreadSession(const std::filesystem::path& credentialDirectory) {
    return std::make_unique<t3detail::StockThreadSession>(credentialDirectory);
}

}
