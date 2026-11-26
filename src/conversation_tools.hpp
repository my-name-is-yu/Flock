#pragma once

#include <cstddef>
#include <filesystem>
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
class ConversationStorage;
class SecretStorage;
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
    ConversationTools(Database& database, AttachmentStorage& attachments, ConversationStorage& conversationTurns, Policy& permissions,
                      const std::filesystem::path& path, BotSettingsStorage& botSettingsStorage, ModelCalls& modelCalls,
                      PolicyApprovals& policyApprovals, SessionReadStorage& sessionReadStorage, SecretStorage& secretStorage)
        : database(database), attachments(attachments), conversationTurns(conversationTurns), permissions(permissions), path(path),
          botSettingsStorage(botSettingsStorage), modelCalls(modelCalls), policyApprovals(policyApprovals),
          sessionReadStorage(sessionReadStorage), secretStorage(secretStorage) {}

    void authorizeBotToolHistory(std::string_view bot, Id message, ModelInput&);
    void saveTurnResults(Id, int step, const Json& results);
    bool runToolBatch(const conversation::Message&, const ToolContext&, int step, const std::vector<ModelCall>&,
                      const ConversationCallContext&);
    void discardPendingResults(Id message);
private:
    class ToolBatch;
    Json workspaceTool(const ToolContext&, const Decision&);
    std::optional<Resource> authorizedBotToolScope(std::string_view bot, const Decision&);
    void validateBotTool(const ToolContext&, const Decision&);
    Json executeBotTool(const ToolContext&, const Decision&);
    std::map<std::pair<Id, int>, std::map<std::size_t, ToolCallResult>> unpersistedToolResults;
    Database& database;
    AttachmentStorage& attachments;
    ConversationStorage& conversationTurns;
    Policy& permissions;
    const std::filesystem::path& path;
    BotSettingsStorage& botSettingsStorage;
    ModelCalls& modelCalls;
    PolicyApprovals& policyApprovals;
    SessionReadStorage& sessionReadStorage;
    SecretStorage& secretStorage;
};
}
