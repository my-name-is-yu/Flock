#pragma once

#include "flock/soul_loader.hpp"
#include "flock/store_options.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Policy;
class ConversationStorage;

class Database;
class SecretRedactor;
class SecretStorage;
struct BotNotes;
struct ModelInput;
struct Person;
struct SessionContext;
struct TaskData;
namespace conversation {
struct Message;
}

struct SessionContextTarget {
    std::string_view bot, displayName, environmentId, threadId;
};

class MemoryStorage {
public:
    MemoryStorage(Database&, const SecretStorage&, Policy&, ConversationStorage&, MemorySeed);

    BotNotes loadNotes(std::string_view bot, const SoulLoader& soul, Seconds now);
    SessionContext sessionContext(const SessionContextTarget&, const SoulLoader&, const SecretRedactor&, std::size_t bytes, Seconds now);
    Json memory(std::string_view bot, const Person&, std::string_view operation, const Json&, Seconds now, bool fromBot = false);
    bool canReviewPreference(const conversation::Message&);
    bool preferenceReviewPending(Id message, int step) const;
    std::optional<ModelInput> preferenceReviewInput(const conversation::Message&, std::vector<std::string>, std::string feedback,
                                                    Seconds now);
    Json rememberPreference(const conversation::Message&, int step, const Json&, Seconds now);
    bool canLearnFrom(const TaskData&);
    void learnFromWork(const TaskData&, Id completion, Id report, std::string_view runId, const Json&, Seconds now);
    void projectMemoryNotice(std::string_view bot, Json& data) const;
private:
    void createMemory(std::string_view key, std::string_view bot, std::string_view id, std::string_view content, Seconds now);
    std::string seedPrivateMemory(std::string_view key, std::string_view bot, const SecretRedactor&, Seconds now);

    Database& database_;
    const SecretStorage& secretStorage_;
    Policy& permissions;
    ConversationStorage& conversationTurns;
    const MemorySeed seed_;
};
}
