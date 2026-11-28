#pragma once

#include <unistd.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace flockv0::test {
// A failed check is a logic_error so that a test expecting a runtime_error from Flock does not swallow it.
inline void check(bool condition, std::string_view message) {
    if (!condition) throw std::logic_error(std::string(message));
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        char name[] = "/tmp/flock-test-XXXXXX";
        const auto* made = mkdtemp(name);
        check(made != nullptr, "Could not create a temporary directory");
        path = made;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
}
