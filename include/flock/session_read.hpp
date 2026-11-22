#pragma once
#include "flock/core.hpp"

#include <algorithm>

namespace flockv0::session_read {
inline bool contains(std::string_view text, std::string_view query) {
    const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; };
    return query.empty() || std::ranges::search(text,query,{},lower,lower).begin() != text.end();
}

inline bool inRange(std::string_view time, std::string_view from, std::string_view to) {
    const auto sortable = [](std::string_view value) {
        std::string key(value.substr(0,19));
        if (value.size() > 20) key += value.substr(20,value.size()-21);
        key.resize(28,'0');
        return key;
    };
    return (from.empty() || sortable(time) >= sortable(from)) &&
           (to.empty() || sortable(time) <= sortable(to));
}
}
