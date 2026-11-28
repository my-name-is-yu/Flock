#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include "flock/types.hpp"
#include "tool_call_result.hpp"

namespace flockv0 {
class BotSettingsStorage;
class ModelCalls;
class Policy;
class PolicyApprovals;
class SessionReadStorage;

class Database;
class AttachmentStorage;
class ConversationTurns;
class ConversationMessages;
class SecretStorage;
class TurnResults;
struct ConversationCallContext;
struct Decision;
struct ModelCall;
struct ModelInput;
struct Resource;
struct ToolContext;
namespace conversation {
struct Message;
}

class ConversationTools {
public:
    ConversationTools(Database& database, AttachmentStorage& attachments, ConversationTurns& conversationTurns,
                      ConversationMessages& conversationMessages, Policy& permissions, const std::filesystem::path& path,
                      BotSettingsStorage& botSettingsStorage, ModelCalls& modelCalls, PolicyApprovals& policyApprovals,
                      SessionReadStorage& sessionReadStorage, SecretStorage& secretStorage, TurnResults& turnResults)
        : database(database), attachments(attachments), conversationTurns(conversationTurns), conversationMessages(conversationMessages),
          permissions(permissions), path(path), botSettingsStorage(botSettingsStorage), modelCalls(modelCalls),
          policyApprovals(policyApprovals), sessionReadStorage(sessionReadStorage), secretStorage(secretStorage), turnResults(turnResults) {
    }

    void authorizeBotToolHistory(std::string_view bot, Id message, ModelInput&);
    // Runs one of the batch's calls that reads the conversation's own records.
    using StoreRead = std::function<std::optional<Json>(const Decision&, std::string_view key)>;
    bool runToolBatch(const conversation::Message&, const ToolContext&, int step, const std::vector<ModelCall>&,
                      const ConversationCallContext&, const StoreRead&);
private:
    class ToolBatch;
    Json workspaceTool(const ToolContext&, const Decision&);
    std::optional<Resource> authorizedBotToolScope(std::string_view bot, const Decision&);
    void validateBotTool(const ToolContext&, const Decision&);
    Json executeBotTool(const ToolContext&, const Decision&);
    Database& database;
    AttachmentStorage& attachments;
    ConversationTurns& conversationTurns;
    ConversationMessages& conversationMessages;
    Policy& permissions;
    const std::filesystem::path& path;
    BotSettingsStorage& botSettingsStorage;
    ModelCalls& modelCalls;
    PolicyApprovals& policyApprovals;
    SessionReadStorage& sessionReadStorage;
    SecretStorage& secretStorage;
    TurnResults& turnResults;
};
}
