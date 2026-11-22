#pragma once

#include "flock/core.hpp"

namespace flockv0::app {

class T3SetupLock {
public:
    explicit T3SetupLock(const std::filesystem::path& state);
    ~T3SetupLock();
    T3SetupLock(const T3SetupLock&) = delete;
    T3SetupLock& operator=(const T3SetupLock&) = delete;

private:
    int fd_;
};

Json configureT3Providers(std::string_view url, std::string_view token);
void removeT3Providers();
void syncT3Skills(const Json& bots);

}
