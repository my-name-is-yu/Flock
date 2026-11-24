#include "storage.hpp"
#include "workspace_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flockv0 {
namespace fs = std::filesystem;
namespace {
constexpr std::size_t maxEntries = 2048;
constexpr std::size_t maxSearchBytes = 1024 * 1024;
constexpr std::size_t maxFileBytes = 8192;
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

struct RegularFile {
    Fd fd;
    std::uintmax_t bytes;
};

RegularFile regularFile(int parent, const fs::path& filename) {
    Fd file(openat(parent,filename.c_str(),O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (fstat(file.get(),&info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
        throw Conflict("Only regular files with one link are readable");
    return {std::move(file),static_cast<std::uintmax_t>(info.st_size)};
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

Json filePreview(int fd, const SecretRedactor& scrub) {
    const auto data = readFile(fd,maxFileBytes+1);
    const auto redacted = safeOutput(scrub.text(std::string_view(data).substr(0,maxFileBytes),data.size() > maxFileBytes));
    return {{"text",bounded(redacted,maxFileBytes)},{"truncated",data.size() > maxFileBytes || redacted.size() > maxFileBytes}};
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
        if (!query.empty()) {
            const auto cap = std::min(maxFileBytes,maxSearchBytes-bytes);
            const auto data = readFile(input.fd.get(),cap+1);
            const auto size = std::min(data.size(),cap);
            bytes += size;
            truncated |= data.size() > size;
            if (std::string_view(data).substr(0,size).find(query) == std::string_view::npos) return true;
        }
        auto name = file.generic_string();
        if (safeOutput(name) == name) files.push_back(std::move(name));
        else truncated = true;
        return true;
    });
    std::ranges::sort(files);
    return {{"files",files},{"truncated",truncated || !complete}};
}

fs::path stateDirectory(const fs::path& database) {
    return fs::canonical(database.has_parent_path() ? database.parent_path() : fs::path("."));
}

Fd openWorkspace(const fs::path& database, const ToolContext& ctx) {
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
    return Fd(openExactChildDirectory(base.get(),ctx.bot));
}

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

Json readToolFile(const fs::path& root, const fs::path& relative, const SecretRedactor& scrub) {
    const auto rootFd = directory(root);
    const auto parent = directoryAt(rootFd.get(),relative.parent_path());
    const auto file = regularFile(parent.get(),relative.filename());
    return filePreview(file.fd.get(),scrub);
}

void Store::Impl::validateWorkspace(const Decision& call) {
    const auto& args = call.args;
    if (call.tool != "workspace_search" || args.contains("path")) {
        const auto relative = relativeToolPath(toolText(args,"path",1024));
        if (relative == "." && call.tool != "workspace_search") throw Conflict("Expected a workspace file");
    }
}

Json Store::Impl::workspaceTool(const ToolContext& ctx, const Decision& call) {
    const auto& args = call.args;
    const auto workspace = openWorkspace(path,ctx);
    const auto root = workspace.get();
    if (call.tool == "workspace_search") {
        auto relative = args.contains("path") ? relativeToolPath(toolText(args,"path",1024)) : fs::path{};
        if (relative == ".") relative.clear();
        return searchWorkspace(root,relative,args.contains("query") ? toolText(args,"query",256) : std::string{});
    }
    const auto relative = relativeToolPath(toolText(args,"path",1024));
    if (call.tool == "workspace_read_file") {
        const auto parent = directoryAt(root,relative.parent_path());
        const auto file = regularFile(parent.get(),relative.filename());
        auto result = filePreview(file.fd.get(),sharedRedactor());
        result["bytes"] = file.bytes;
        return result;
    }
    if (call.tool == "workspace_write_file") {
        const auto data = toolText(args,"text",maxFileBytes,true);
        writeFile(root,relative,data);
        return {{"bytes",data.size()}};
    }
    throw Conflict("Unknown workspace tool");
}
}
