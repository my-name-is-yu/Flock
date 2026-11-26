#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>

namespace flockv0 {
struct StoreOptions;

// Separate CLI and Web connections may use the same database.
class Store {
public:
    explicit Store(std::filesystem::path database);
    Store(std::filesystem::path database, StoreOptions);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // A change hint for readers, covering writes on this connection and other connections.
    std::pair<std::int64_t, std::int64_t> changeStamp() const;
    const std::filesystem::path& path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    friend class StoreAccess;
};
}
