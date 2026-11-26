#pragma once

#include "flock/store_options.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <functional>
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
struct Command;
struct ModelInput;
struct Person;
struct SessionMemory;
namespace conversation {
struct Message;
}

class MemoryStorage {
public:
    MemoryStorage(Database&, const SecretStorage&, Policy&, ConversationStorage&, MemorySeed);

    BotNotes loadNotes(std::string_view bot, const std::function<std::string(std::string_view)>& soul, Seconds now);
    std::optional<SessionMemory> sessionMemory(const Command&, Seconds now);
    Json memory(std::string_view bot, const Person&, std::string_view operation, const Json&, Seconds now, bool fromBot = false);
    bool canReviewPreference(const conversation::Message&);
    bool preferenceReviewPending(Id message, int step) const;
    std::optional<ModelInput> preferenceReviewInput(const conversation::Message&, std::vector<std::string>, std::string feedback,
                                                    Seconds now);
    Json rememberPreference(const conversation::Message&, int step, const Json&, Seconds now);
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
