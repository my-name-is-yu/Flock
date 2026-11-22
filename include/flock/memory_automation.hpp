#pragma once

#include "flock/core.hpp"

#include <bitset>

namespace date { class time_zone; }

namespace flockv0 {
struct AccessDenied : Conflict { using Conflict::Conflict; };

class Cron {
public:
    Cron(std::string_view expression, std::string_view timezone);
    bool matches(Seconds utc) const;
    Seconds next(Seconds after) const;
private:
    bool matchesDay(unsigned month, unsigned day, unsigned weekday) const;
    std::bitset<60> minutes_, hours_, days_, months_, weekdays_;
    bool anyDay_, anyWeekday_;
    const date::time_zone* zone_;
};

struct ConfiguredSchedule {
    std::string id, expression, timezone;
    Intake routing;
};
}
