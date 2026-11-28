#include "flock/private_files.hpp"
#include "flock/file_descriptor.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>

namespace flockv0 {
namespace {
bool writeAll(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = write(fd, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}
}

bool ownerOnly(int fd, bool directory) {
    struct stat info{};
    if (fd < 0 || fstat(fd, &info) != 0 || info.st_uid != geteuid() || (info.st_mode & 0077)) return false;
    return directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode) && info.st_nlink == 1;
}

bool replacePrivateFile(int directory, const std::string& temporary, const std::string& name, std::string_view bytes) {
    const FileDescriptor file(openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.get() < 0) return false;
    const bool replaced = ownerOnly(file.get(), false) && writeAll(file.get(), bytes) && fsync(file.get()) == 0 &&
                          renameat(directory, temporary.c_str(), directory, name.c_str()) == 0;
    if (!replaced) {
        unlinkat(directory, temporary.c_str(), 0);
        return false;
    }
    return fsync(directory) == 0 || errno == EINVAL || errno == ENOTSUP;
}
}
