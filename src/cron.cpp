#include "flock/memory_automation.hpp"
#include "flock/types.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace flockv0 {
namespace {
cctz::time_zone locateZone(std::string_view name) {
    constexpr std::string_view allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_+-/";
    cctz::time_zone zone;
    if (name.empty() || name.starts_with("/") || name.ends_with("/") || name.find("//") != name.npos ||
        name.find_first_not_of(allowed) != name.npos || name == "localtime" || name == "posixrules" || name.starts_with("Fixed/") ||
        name.starts_with("posix/") || name.starts_with("right/") || !cctz::load_time_zone(std::string(name), &zone))
        throw Conflict("Unknown IANA timezone: " + std::string(name));
    return zone;
}

int number(std::string_view value) {
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size()) throw Conflict("Invalid number in cron expression");
    return result;
}

std::bitset<60> cronField(std::string_view input, int low, int high, bool weekday = false) {
    std::bitset<60> values;
    for (;;) {
        const auto comma = input.find(',');
        const auto entry = input.substr(0, comma);
        const auto slash = entry.find('/');
        const auto range = entry.substr(0, slash);
        const int step = slash == std::string_view::npos ? 1 : number(entry.substr(slash + 1));
        int first = low, last = high;
        if (range != "*") {
            const auto dash = range.find('-');
            first = number(range.substr(0, dash));
            last = dash != std::string_view::npos ? number(range.substr(dash + 1)) : (slash != std::string_view::npos ? high : first);
        }
        if (first < low || last > high || first > last || step < 1 || step > high - low + 1)
            throw Conflict("Cron field is outside its allowed range");
        for (int n = first; n <= last; n += step) values.set(weekday && n == 7 ? 0 : n);
        if (comma == std::string_view::npos) return values;
        input.remove_prefix(comma + 1);
    }
}
}

void validateTimezone(std::string_view name) {
    locateZone(name);
}

Cron::Cron(std::string_view expression, std::string_view timezone) : zone_(locateZone(timezone)) {
    std::istringstream fields{std::string(expression)};
    std::array<std::string, 5> value;
    for (auto& field : value)
        if (!(fields >> field)) throw Conflict("Cron requires five fields");
    std::string extra;
    if (fields >> extra) throw Conflict("Cron requires five fields");
    const auto minutes = cronField(value[0], 0, 59);
    const auto hours = cronField(value[1], 0, 23);
    for (int hour = 0; hour < 24; ++hour)
        if (hours.test(hour))
            for (int minute = 0; minute < 60; ++minute)
                if (minutes.test(minute)) times_.push_back(hour * 60 + minute);
    days_ = cronField(value[2], 1, 31);
    months_ = cronField(value[3], 1, 12);
    weekdays_ = cronField(value[4], 0, 7, true);
    anyDay_ = value[2].starts_with("*");
    anyWeekday_ = value[4].starts_with("*");
}

bool Cron::matchesDay(unsigned month, unsigned day, unsigned weekday) const {
    const bool dom = days_.test(day), dow = weekdays_.test(weekday);
    return months_.test(month) && (anyDay_ || anyWeekday_ ? dom && dow : dom || dow);
}

Cron::Occurrences Cron::scan(Seconds from, Seconds through, bool stopAtFirst) const {
    using namespace std::chrono;
    Occurrences result;
    auto cursor = sys_seconds{seconds{from}};
    const auto end = sys_seconds{seconds{through + 1}};
    while (cursor < end) {
        const auto offset = seconds{zone_.lookup(cursor).offset};
        cctz::time_zone::civil_transition transition;
        auto segmentEnd = end;
        if (zone_.next_transition(cursor, &transition)) segmentEnd = std::min(zone_.lookup(transition.to).trans, end);
        const auto localStart = ceil<minutes>(local_seconds{cursor.time_since_epoch() + offset});
        const auto localEnd = ceil<minutes>(local_seconds{segmentEnd.time_since_epoch() + offset});
        for (auto day = floor<days>(localStart); day < localEnd; day += days{1}) {
            const year_month_day ymd{sys_days{day.time_since_epoch()}};
            if (!matchesDay(unsigned(ymd.month()), unsigned(ymd.day()), weekday{day}.c_encoding())) continue;
            const auto startMinute = (std::max(localStart, local_time<minutes>{day}) - day).count();
            const auto endMinute = std::min<std::int64_t>(1440, (localEnd - day).count());
            const auto first = std::lower_bound(times_.begin(), times_.end(), startMinute);
            const auto last = std::lower_bound(first, times_.end(), endMinute);
            if (first == last) continue;
            const auto utc = [&](int minute) {
                return duration_cast<seconds>((day + minutes{minute}).time_since_epoch() - offset).count();
            };
            if (!result.count) result.first = utc(*first);
            if (stopAtFirst) return {result.first, result.first, 1};
            result.last = utc(*std::prev(last));
            result.count += last - first;
        }
        cursor = segmentEnd;
    }
    return result;
}

std::optional<Seconds> Cron::next(Seconds after) const {
    if (after >= lastAutomationTime) return std::nullopt;
    const auto found = scan(after + 1, lastAutomationTime, true);
    return found.count ? std::optional{found.first} : std::nullopt;
}

Cron::Occurrences Cron::between(Seconds from, Seconds through) const {
    return scan(from, through, false);
}
}
