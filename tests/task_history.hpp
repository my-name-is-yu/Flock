#pragma once

#include "flock/core.hpp"

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
