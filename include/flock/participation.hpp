#pragma once

#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <string>
#include <string_view>

namespace flockv0 {
class Store;
class T3;
struct Person;
struct T3Connection;

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
