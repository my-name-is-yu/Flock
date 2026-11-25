#pragma once

#include "storage.hpp"
#include "journal_peer.hpp"

namespace flockv0 {
class Runtime::SessionHistory {
public:
    enum class PrefillResult { NotNeeded, Persisted, UnavailableWithSettledReport };
    static constexpr std::size_t maxEvidenceReads = 3;

    SessionHistory(Store::Impl&, T3&, const Policy&, TaskUpdates&, const std::string& mode);
    static std::size_t readOffset(const Json& args);
    SessionExcerptEntry readExcerpt(std::string_view bot, const ReadRequest& req);
    bool readEvidence(const TaskData&, const Json& event, const Json& args, const Json& usage = Json::object());
    void appendEventEvidence(const TaskData&, const Json& event, const Json& entry);
    void recordSessionRead(const TaskData&, const Json& event, Json entry, const Json& usage);
    PrefillResult prefillHistory(const TaskData&, const Json& event);
private:
    Store::Impl& db;
    T3& t3;
    const Policy& policy;
    TaskUpdates& updates;
    const std::string& mode;
};
}
