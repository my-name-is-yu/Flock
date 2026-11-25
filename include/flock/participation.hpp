#pragma once

#include "flock/t3_connection.hpp"

namespace flockv0 {

struct SessionAsk {
    std::string bot, text, threadId, requestId;
};

struct SessionStopResult {
    Id cancelled = 0, alreadyStarted = 0, notCancellable = 0, pending = 0, stoppedChildSessions = 0;
    Json data() const;
    std::string summary() const;
};

class SessionParticipation {
public:
    SessionParticipation(Store& store, T3& t3) : store_(store), t3_(t3) {}
    Person person(const T3Connection&) const;
    Id ask(const T3Connection&, const SessionAsk&, const IntakeResolver&);
    SessionStopResult stop(const T3Connection&, std::string_view bot, std::string_view threadId);
private:
    Store& store_;
    T3& t3_;
};

}
