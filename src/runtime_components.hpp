#pragma once

#include "flock/runtime.hpp"

namespace flockv0 {
struct Runtime::Components {
    class Maintenance;
    class TaskScheduler;
    class TaskTurn;
    class TaskDecisions;
    class OutboxDispatch;
    class SessionPrompts;
    class RunObserver;
    class PeerExchanges;
    class RunReports;
    class SessionHistory;
    class TaskQuestions;
    class TaskUpdates;
    class DatabaseLock;
    class Workers;
};
}
