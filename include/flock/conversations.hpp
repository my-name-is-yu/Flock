#pragma once

#include "flock/json.hpp"
#include "flock/task_attachment.hpp"
#include "flock/types.hpp"
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Store;
class ChannelStorage;
class ConversationContextStorage;
class Database;
class MemoryStorage;
struct Person;

class Conversations {
public:
    explicit Conversations(Store&);

    Json compactions(std::string_view bot, Id before = 0, unsigned limit = 50) const;
    // One page of chronological user-visible conversation, across this Bot's tasks.
    enum class ConversationEvents { all, incoming };
    Json conversation(std::string_view botId, Id before = 0, unsigned limit = 50, ConversationEvents events = ConversationEvents::all,
                      const Person* viewer = nullptr) const;
    std::optional<AttachmentFile> attachment(const Person&, std::string_view bot, Id message, Id attachment) const;
    struct ConversationChanges {
        Id through;
        std::map<std::string, std::vector<Id>> messages;
    };
    // Original messages affected by replies and instruction delivery since a journal cursor.
    ConversationChanges conversationChangesSince(std::optional<Id> after, std::string_view bot = {}) const;
private:
    Store& store_;
    Database& database_;
    const ConversationContextStorage& context_;
    const MemoryStorage& memory_;
    const ChannelStorage& channels_;
};
}
