#pragma once

#include <unistd.h>

#include <utility>

namespace flockv0 {
// Owns a POSIX file descriptor. A negative value owns nothing.
class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        std::swap(fd_, other.fd_);
        return *this;
    }
    ~FileDescriptor() { close(); }

    int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
private:
    int fd_;
};
}
