#pragma once

#include "flock/json.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0::app {

class T3SetupLock {
public:
    T3SetupLock();
    ~T3SetupLock();
    T3SetupLock(const T3SetupLock&) = delete;
    T3SetupLock& operator=(const T3SetupLock&) = delete;
private:
    int fd_;
};

using InstallT3Provider = std::function<void(std::string_view)>;
std::vector<std::string> configureT3Providers(std::string_view url, const std::function<void(const InstallT3Provider&)>& connect,
                                              const std::function<bool(std::string_view)>& owns = {});
bool t3ProvidersNeedRefresh(const std::function<bool(std::string_view)>& owns);
void removeT3Providers(const std::function<bool(std::string_view)>& owns);

}
