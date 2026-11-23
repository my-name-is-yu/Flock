#include "settings_file.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace flockv0::app {
namespace {
constexpr std::size_t maxSettingsBytes = 512 * 1024;

struct File {
    int fd;
    ~File() { if (fd >= 0) ::close(fd); }
};

struct PendingSettings {
    std::vector<char> name;

    explicit PendingSettings(const std::filesystem::path& path) {
        auto pattern = path.string() + ".XXXXXX";
        name.assign(pattern.cbegin(),pattern.cend());
        name.push_back('\0');
    }
    ~PendingSettings() { if (!name.empty()) ::unlink(name.data()); }
};
}

Json readSettingsFile(const std::filesystem::path& path, Json fallback) {
    const File file{::open(path.c_str(),O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (file.fd < 0 && errno == ENOENT) return fallback;
    struct stat info{};
    if (file.fd < 0 || ::fstat(file.fd,&info) || !S_ISREG(info.st_mode) ||
        info.st_uid != ::geteuid() || (info.st_mode & 07777) != 0600 ||
        info.st_size <= 0 || static_cast<std::size_t>(info.st_size) > maxSettingsBytes)
        throw Conflict("Flock settings require a private, owner-only file.");
    std::string bytes(static_cast<std::size_t>(info.st_size),'\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(file.fd,bytes.data() + offset,bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw Conflict("Could not read the saved settings.");
        offset += static_cast<std::size_t>(count);
    }
    auto result = Json::parse(bytes,nullptr,false);
    if (!result.is_object()) throw Conflict("The saved settings are invalid.");
    return result;
}

void writeSettingsFile(const std::filesystem::path& path, const Json& value) {
    const auto bytes = value.dump(2) + '\n';
    if (bytes.size() > maxSettingsBytes) throw Conflict("The settings are too large to save.");
    writePrivateFile(path,bytes);
}

void writePrivateFile(const std::filesystem::path& path, std::string_view bytes) {
    PendingSettings pending(path);
    const File file{::mkstemp(pending.name.data())};
    if (file.fd < 0) {
        pending.name.clear();
        throw Conflict("Could not create a private settings file.");
    }
    if (::fcntl(file.fd,F_SETFD,FD_CLOEXEC) < 0 || ::fchmod(file.fd,0600))
        throw Conflict("Could not protect the settings file.");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(file.fd,bytes.data() + offset,bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw Conflict("Could not save the settings.");
        offset += static_cast<std::size_t>(count);
    }
    if (::fsync(file.fd) || ::rename(pending.name.data(),path.c_str()))
        throw Conflict("Could not save the settings.");
    pending.name.clear();
}
}
