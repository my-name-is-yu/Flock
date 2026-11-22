#pragma once
#include "flock/core.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>

namespace flockv0::session_read {
inline bool contains(std::string_view text, std::string_view query) {
    const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; };
    return query.empty() || std::ranges::search(text,query,{},lower,lower).begin() != text.end();
}

inline bool utcTimestamp(std::string_view value) {
    if (value.size() < 20 || value.size() > 30 || value.back() != 'Z' ||
        value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':') return false;
    if (value.size() != 20 && (value.size() < 22 || value[19] != '.' ||
        value.substr(20,value.size()-21).find_first_not_of("0123456789") != value.npos)) return false;
    const auto read = [&](std::size_t start, std::size_t length, int maximum) {
        const auto part = value.substr(start,length);
        int number = 0;
        const auto [end,error] = std::from_chars(part.data(),part.data()+length,number);
        return error == std::errc() && end == part.data()+length && part.front() != '-' &&
            number <= maximum ? number : -1;
    };
    const auto year = read(0,4,9999);
    using namespace std::chrono;
    return year >= 0 && (std::chrono::year{year}/read(5,2,12)/read(8,2,31)).ok() &&
        read(11,2,23) >= 0 && read(14,2,59) >= 0 && read(17,2,59) >= 0;
}

inline int compareTime(std::string_view a, std::string_view b) {
    if (const auto seconds = a.substr(0,19).compare(b.substr(0,19)); seconds != 0) return seconds;
    for (std::size_t i = 20; i < 29; ++i) {
        const char lhs = i+1 < a.size() ? a[i] : '0';
        const char rhs = i+1 < b.size() ? b[i] : '0';
        if (lhs != rhs) return lhs - rhs;
    }
    return 0;
}

inline bool inRange(std::string_view time, std::string_view from, std::string_view to) {
    return (from.empty() || compareTime(time,from) >= 0) && (to.empty() || compareTime(time,to) <= 0);
}
}
