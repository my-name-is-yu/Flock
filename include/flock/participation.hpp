#pragma once

#include "flock/t3_connection.hpp"

namespace flockv0 {

struct SessionAsk {
    std::string bot, text, threadId, requestId;
};

class SessionParticipation {
public:
    SessionParticipation(Store& store, T3& t3) : store_(store), t3_(t3) {}
    Person person(const T3Connection&) const;
    Id ask(const T3Connection&, const SessionAsk&, const IntakeResolver&);
    void stop(const T3Connection&, std::string_view bot, std::string_view threadId);

private:
    Store& store_;
    T3& t3_;
};

}
