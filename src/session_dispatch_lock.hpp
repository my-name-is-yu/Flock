#pragma once

#include <filesystem>

namespace flockv0 {
class SessionDispatchFileLock {
public:
    enum class Mode { shared, exclusive };
    SessionDispatchFileLock(const std::filesystem::path& database, Mode);
    ~SessionDispatchFileLock();
    SessionDispatchFileLock(const SessionDispatchFileLock&) = delete;
    SessionDispatchFileLock& operator=(const SessionDispatchFileLock&) = delete;
private:
    int fd_;
};
}
