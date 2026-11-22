#pragma once

#include "flock/core.hpp"

#include <bitset>
#include <chrono>
#include <cctz/time_zone.h>

namespace flockv0 {
struct AccessDenied : Conflict { using Conflict::Conflict; };

inline constexpr Seconds lastAutomationTime = std::chrono::sys_seconds{
    std::chrono::sys_days{std::chrono::year{2200}/1/1}
}.time_since_epoch().count() - 1;
void validateTimezone(std::string_view);

class Cron {
public:
    Cron(std::string_view expression, std::string_view timezone);
    struct Occurrences {
        Seconds first = 0;
        Seconds last = 0;
        Id count = 0;
    };

    std::optional<Seconds> next(Seconds after) const;
    Occurrences between(Seconds from, Seconds through) const;
private:
    bool matchesDay(unsigned month, unsigned day, unsigned weekday) const;
    Occurrences scan(Seconds from, Seconds through, bool stopAtFirst) const;
    std::vector<int> times_;
    std::bitset<60> days_, months_, weekdays_;
    bool anyDay_, anyWeekday_;
    cctz::time_zone zone_;
};

struct ConfiguredSchedule {
    std::string id, expression, timezone;
    Intake routing;
};
}
