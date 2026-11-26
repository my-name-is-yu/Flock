#pragma once

#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "flock/json.hpp"
#include "flock/session_ref.hpp"
#include "flock/store.hpp"
#include "flock/types.hpp"

namespace flockv0 {

class Database;
class PeerStorage;
class SecretStorage;
class WaitStorage;
class T3;
struct T3Project;
struct TaskData;
struct WorkSession;
namespace conversation {
struct Message;
}

std::string conversationEnvironmentId(const Json& alias);
std::string conversationGatewayEnvironmentId(const Json& alias);
std::string configuredT3Project(const Json& routing, std::string_view environment, std::string_view name = {});

struct LinkedTaskDestinations {
    std::vector<RunTarget> choices;
    std::vector<std::string> unavailable;
};
struct StartDestinations {
    TaskRoute primary;
    std::vector<RunTarget> targets;
    std::string commit;
};
struct TaskChange {
    std::string intent, doneWhen;
    TaskRoute route;
};
using ProjectCatalog = std::variant<std::vector<T3Project>, std::string>;

class SessionReadStorage;
class SessionParticipationStorage;
class TaskRouting {
public:
    TaskRouting(Database& database, Store::Impl& remaining, PeerStorage& peerStorage, SecretStorage& secretStorage,
                WaitStorage& waitStorage, SessionReadStorage& sessionReadStorage, SessionParticipationStorage& sessionParticipationStorage)
        : database(database), remaining(remaining), peerStorage(peerStorage), secretStorage(secretStorage), waitStorage(waitStorage),
          sessionReadStorage(sessionReadStorage), sessionParticipationStorage(sessionParticipationStorage) {}

    bool automationBeforeDispatch(Id task) const;
    bool isUndispatchedNotesWait(const TaskData& task) const;
    bool projectRouteReady(Id task, std::string_view action) const;
    std::string rememberedT3Project(std::string_view bot, std::string_view environment) const;
    void rememberT3Project(std::string_view bot, std::string_view environment, std::string_view project);
    bool projectQuestionOpen(std::string_view bot, std::string_view environment) const;
    ProjectCatalog readProjectCatalog(T3&, std::string_view bot, std::string_view environment, bool requireConnection);
    bool applyProjectCatalog(TaskData& task, ProjectCatalog catalog);
    std::set<std::string> registeredT3Environments(std::string_view bot);
    std::string taskEnvironment(std::string_view bot, const Json& routing, std::string_view name = {});
    TaskRoute taskRoute(std::string_view bot, const Json& routing, std::string_view name, const TaskData* movingTask = nullptr,
                        const LinkedTaskDestinations* linked = nullptr);
    StartDestinations startDestinations(const conversation::Message&, const Json& args, bool resume, const WorkSession* selectedSession,
                                        const LinkedTaskDestinations& linked);
    LinkedTaskDestinations discoverLinkedTaskDestinations(T3&, std::string_view bot);
    TaskChange taskChange(const conversation::Message&, const Json& args, const TaskData& task, const LinkedTaskDestinations& linked);
    std::string defaultT3Environment(std::string_view bot, std::string_view configured, std::string_view preferred);
private:
    Database& database;
    Store::Impl& remaining;
    SessionReadStorage& sessionReadStorage;
    SessionParticipationStorage& sessionParticipationStorage;
    PeerStorage& peerStorage;
    SecretStorage& secretStorage;
    WaitStorage& waitStorage;
};
}
