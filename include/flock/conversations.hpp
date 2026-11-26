#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Store;
class ChannelStorage;
class ConversationContextStorage;
class ConversationStorage;
class Database;
class MemoryStorage;
class ModelCalls;
class Policy;
struct Intake;
struct Person;

class Conversations {
public:
    explicit Conversations(Store&);

    // Enqueues a Bot message for Runtime. Retrying the same key and payload returns the same receipt.
    Json receive(const Person&, const Intake& routing, std::string_view clientMessageId, std::string_view text, Id replyToMessageId = 0,
                 const Json& attachments = Json::array());
    // Refers to a person in the current Web identity directory when the Runtime executes the turn.
    Json receiveWeb(const Person&, const Intake& routing, std::string_view clientMessageId, std::string_view text, Id replyToMessageId = 0,
                    const Json& attachments = Json::array());
    std::optional<Json> receipt(const Person&, std::string_view botId, std::string_view clientMessageId, std::string_view text,
                                Id replyToMessageId = 0, const Json& attachments = Json::array()) const;
    // Conversation turns paused on the model call limit, newest first.
    Json pausedConversations(std::string_view botId) const;
    Json compactions(std::string_view bot, Id before = 0, unsigned limit = 50) const;
    // One page of chronological user-visible conversation, across this Bot's tasks.
    enum class ConversationEvents { all, incoming };
    Json conversation(std::string_view botId, Id before = 0, unsigned limit = 50, ConversationEvents events = ConversationEvents::all,
                      std::string_view viewer = {}) const;
    struct ConversationChanges {
        Id through;
        std::map<std::string, std::vector<Id>> messages;
    };
    // Original messages affected by replies and instruction delivery since a journal cursor.
    ConversationChanges conversationChangesSince(std::optional<Id> after, std::string_view bot = {}) const;
private:
    Database& database_;
    ConversationStorage& turns_;
    const ConversationContextStorage& context_;
    const MemoryStorage& memory_;
    const ModelCalls& modelCalls_;
    const ChannelStorage& channels_;
    const Policy& permissions_;
};
}
