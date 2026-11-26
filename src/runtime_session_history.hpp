#pragma once

#include "runtime_components.hpp"
#include "journal_peer.hpp"
#include "flock/json.hpp"
#include "flock/t3_session.hpp"
#include "flock/task.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace flockv0 {
class Database;
class JournalStorage;
class Policy;
class SecretStorage;

class Runtime::Components::SessionHistory {
public:
    enum class PrefillResult { NotNeeded, Persisted, UnavailableWithSettledReport };
    static constexpr std::size_t maxEvidenceReads = 3;

    SessionHistory(Database&, JournalStorage&, SecretStorage&, T3&, const Policy&, TaskUpdates&, const std::string& mode);
    static std::size_t readOffset(const Json& args);
    SessionExcerptEntry readExcerpt(std::string_view bot, const ReadRequest& req);
    bool readEvidence(const TaskData&, const Json& event, const Json& args, const Json& usage = Json::object());
    void appendEventEvidence(const TaskData&, const Json& event, const Json& entry);
    void recordSessionRead(const TaskData&, const Json& event, Json entry, const Json& usage);
    PrefillResult prefillHistory(const TaskData&, const Json& event);
private:
    Database& database;
    JournalStorage& journalStorage;
    SecretStorage& secretStorage;
    T3& t3;
    const Policy& policy;
    TaskUpdates& updates;
    const std::string& mode;
};
}
