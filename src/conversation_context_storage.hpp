#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <cstddef>
#include <string_view>

namespace flockv0 {
class AttachmentStorage;
class Database;
class SecretStorage;
namespace conversation {
struct Message;
}

class ConversationContextStorage {
public:
    ConversationContextStorage(Database&, const SecretStorage&, const AttachmentStorage&);

    Json compactions(std::string_view bot, Id before, unsigned limit, Id beforeMessage = 0) const;
    static std::size_t boundExpandedContext(Json& turn);
    Json conversationHistory(const conversation::Message&, const Json& summaries, std::size_t expandedBytes) const;
    Json contextTool(std::string_view tool, const Json&, const conversation::Message&, int step);
private:
    Database& database_;
    const SecretStorage& secretStorage_;
    const AttachmentStorage& attachments_;
};
}
