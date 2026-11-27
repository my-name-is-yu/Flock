#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <utility>
#include <vector>
#include "flock/json.hpp"
#include "flock/session_ref.hpp"
#include "flock/task.hpp"
#include "flock/t3.hpp"
#include "flock/types.hpp"

namespace flockv0 {

class Database;
class Policy;
class WaitStorage;
class T3;
struct TaskData;
class SecretRedactor;
struct WorkSession;
namespace conversation {
struct Message;
}

using ProjectCatalog = std::variant<std::vector<T3Project>, std::string>;
Json projectChoices(const std::vector<T3Project>&, const SecretRedactor&);

struct TaskDestinations {
    std::vector<RunTarget> direct;
    std::vector<RunTarget> linked;
    std::vector<std::string> unavailable;
    std::map<std::pair<std::string, std::string>, std::vector<T3Project>> catalogs;
    // What each link lets Sessions do without asking, as T3 reports it, by linked environment and gateway.
    std::map<std::pair<std::string, std::string>, std::string> linkAccess;

    std::string access(const TaskRoute& route) const {
        const auto found = linkAccess.find({route.environmentId, route.gatewayEnvironmentId});
        return found == linkAccess.end() ? std::string{} : found->second;
    }

    const std::vector<T3Project>* projects(const TaskRoute&) const;
    void chooseProject(TaskRoute&, std::string_view project) const;
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
class SessionReadStorage;
class SessionParticipationStorage;
class TaskRouting {
public:
    TaskRouting(Database& database, const Policy& permissions, WaitStorage& waitStorage, SessionReadStorage& sessionReadStorage,
                SessionParticipationStorage& sessionParticipationStorage)
        : database(database), permissions(permissions), waitStorage(waitStorage), sessionReadStorage(sessionReadStorage),
          sessionParticipationStorage(sessionParticipationStorage) {}

    bool automationBeforeDispatch(Id task) const;
    bool isUndispatchedNotesWait(const TaskData& task) const;
    bool projectRouteReady(Id task, std::string_view action) const;
    std::string rememberedT3Project(std::string_view bot, std::string_view environment) const;
    void rememberT3Project(std::string_view bot, std::string_view environment, std::string_view project);
    ProjectCatalog readProjectCatalog(T3&, std::string_view bot, const TaskRoute&, bool requireConnection);
    void applyProjectCatalog(TaskData& task, const ProjectCatalog& catalog);
    void assignProject(TaskData&, std::string_view project);
    std::set<std::string> registeredT3Environments(std::string_view bot);
    std::string taskEnvironment(std::string_view bot, const Json& routing, std::string_view name = {});
    TaskRoute taskRoute(std::string_view bot, const Json& routing, std::string_view name, const TaskData* movingTask = nullptr,
                        const TaskDestinations* linked = nullptr);
    StartDestinations startDestinations(const conversation::Message&, const Json& args, bool resume, const WorkSession* selectedSession,
                                        const TaskDestinations& linked);
    TaskDestinations discoverTaskDestinations(T3&, const conversation::Message&, bool listProjects, const Json* change = nullptr);
    TaskChange taskChange(const conversation::Message&, const Json& args, const TaskData& task, const TaskDestinations& linked);
    std::string defaultT3Environment(std::string_view bot, std::string_view configured, std::string_view preferred);
private:
    Database& database;
    const Policy& permissions;
    WaitStorage& waitStorage;
    SessionReadStorage& sessionReadStorage;
    SessionParticipationStorage& sessionParticipationStorage;
};
}
