#pragma once

#include "flock/json.hpp"
#include "flock/store.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <string_view>

namespace flockv0::test {
inline Json lastTaskEvent(const Store& store, Id task, std::string_view kind) {
    constexpr unsigned pageSize = 200;
    Id after = 0;
    Json result = nullptr;
    while (true) {
        const auto page = store.history(task, after, pageSize);
        for (const auto& entry : page) {
            if (entry.at("kind") == kind) result = entry;
        }
        if (page.size() < pageSize) return result;
        after = page.back().at("id");
    }
}
}
