#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Store;
class ChannelStorage;
class ConversationContextStorage;
class Database;
class MemoryStorage;
class ModelCalls;
class SuggestionStorage;

class Conversations {
public:
    explicit Conversations(Store&);

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
    const ConversationContextStorage& context_;
    const MemoryStorage& memory_;
    const ModelCalls& modelCalls_;
    const ChannelStorage& channels_;
    const SuggestionStorage& suggestions_;
};
}
