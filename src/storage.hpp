#pragma once
#include "task_interaction_storage.hpp"
#include "database.hpp"
#include "session_message_storage.hpp"
#include "conversation_tools.hpp"
#include "conversation_turns.hpp"
#include "bot_settings_storage.hpp"
#include "policy_approval_types.hpp"
#include "session_send_receipt.hpp"
#include "conversation_checkpoint.hpp"
#include "conversation_call.hpp"
#include "session_stop.hpp"
#include "task_run_reports.hpp"
#include "task_routing.hpp"
#include "task_operations.hpp"
#include "model_calls.hpp"
#include "git_storage.hpp"
#include "inbox_storage.hpp"
#include "policy_origin.hpp"
#include "conversation_context_storage.hpp"
#include "workflow_storage.hpp"
#include "session_participation_storage.hpp"
#include "session_read_storage.hpp"
#include "tool_call_result.hpp"
#include "json_fields.hpp"
#include "wait_storage.hpp"
#include "task_records.hpp"
#include "task_events.hpp"
#include "journal_storage.hpp"
#include "automation_storage.hpp"
#include "memory_storage.hpp"
#include "attachment_storage.hpp"
#include "bot_lifecycle_storage.hpp"
#include "peer_storage.hpp"
#include "channel_storage.hpp"
#include "local_machine_storage.hpp"
#include "outbox_storage.hpp"
#include "run_ref_storage.hpp"
#include "secret_storage.hpp"
#include "policy_approvals.hpp"
#include "policy_origin.hpp"
#include "web_identity_state.hpp"
#include "flock/policy.hpp"
#include "flock/secrets.hpp"
#include "workspace_internal.hpp"
#include "wait_storage.hpp"
#include "flock/board_inbox.hpp"
#include "flock/git_host.hpp"
#include "flock/channels.hpp"
#include "flock/outbox.hpp"
#include "flock/types.hpp"
#include "flock/intake.hpp"
#include "flock/person.hpp"
#include "flock/store.hpp"
#include "flock/bot_change.hpp"
#include "flock/json.hpp"
#include "flock/model_error.hpp"
#include "flock/t3_command.hpp"
#include "flock/decision.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "flock/session_ref.hpp"
#include "flock/wait.hpp"
#include "flock/outbox_instruction.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

namespace flockv0 {
class Model;
class T3;
struct BotNotes;
struct ModelImage;
struct ModelInput;
struct Observation;
struct T3Project;

namespace conversation {
struct Message;
struct QuestionAnchor;
}

inline bool hasDestinationSelector(const Json& args) {
    return args.is_object() && (args.contains("destination") || args.contains("environmentId") || args.contains("projectId") ||
                                args.contains("gatewayEnvironmentId"));
}

Id requiredInteger(const Json&, std::string_view key, Id minimum = 1);
std::string redactT3Tokens(std::string);

struct CompletionEntry;
struct ToolErrorEntry;
struct DecisionEntry;
struct ConversationInvitationEntry;
struct ConversationMessageEntry;
struct ConversationReplyEntry;
enum class ConversationReplyRoute;
struct TaskRevisionEntry;
struct DeliveredInstructionEntry;
struct UserAnswerEntry;
struct SendStartedEntry;
struct RunReportEntry;
struct ConversationInterruptEntry;
struct BotMessageEntry;
struct PeerEvidenceEntry;
struct PrReviewRequestedEntry;
struct GitPrFollowupEntry;

struct Store::Impl : Database {
    BotSettingsStorage botSettingsStorage{*this, permissions, sessionParticipationStorage, taskRouting};
    ConversationStorage conversationTurns{*this,
                                          attachments,
                                          automationStorage,
                                          botLifecycle,
                                          botSettingsStorage,
                                          channelStorage,
                                          conversationContextStorage,
                                          conversationTools,
                                          gitStorage,
                                          permissions,
                                          journalStorage,
                                          localMachineStorage,
                                          memoryStorage,
                                          modelCalls,
                                          peerStorage,
                                          policyApprovals,
                                          secretStorage,
                                          sessionMessageStorage,
                                          sessionParticipationStorage,
                                          sessionReadStorage,
                                          taskOperations,
                                          taskRecords,
                                          taskRouting,
                                          waitStorage,
                                          webIdentity,
                                          workflowStorage};
    ConversationTools conversationTools{*this,      attachments,     conversationTurns,  permissions,  path, botSettingsStorage,
                                        modelCalls, policyApprovals, sessionReadStorage, secretStorage};
    SessionMessageStorage sessionMessageStorage{*this,
                                                botLifecycle,
                                                conversationTurns,
                                                conversationTools,
                                                gitStorage,
                                                permissions,
                                                path,
                                                journalStorage,
                                                outboxStorage,
                                                policyApprovals,
                                                secretStorage,
                                                sessionParticipationStorage,
                                                sessionReadStorage,
                                                taskRecords,
                                                taskRunReports};
    ModelCalls modelCalls{*this, botLifecycle, *this, journalStorage, secretStorage, taskRecords, waitStorage, channelStorage};
    TaskOperations taskOperations{*this,       automationStorage, botLifecycle, *this,       journalStorage, modelCalls,  outboxStorage,
                                  peerStorage, secretStorage,     taskEvents,   taskRecords, taskRouting,    waitStorage, inboxStorage};
    TaskRouting taskRouting{*this, *this, peerStorage, secretStorage, waitStorage, sessionReadStorage, sessionParticipationStorage};
    TaskRunReports taskRunReports{*this, policyApprovals, journalStorage, outboxStorage, waitStorage};
    OutboxStorage outboxStorage{*this};
    PeerStorage peerStorage{*this};
    JournalStorage journalStorage{*this, channelStorage};
    TaskEvents taskEvents{*this, modelCalls};
    WaitStorage waitStorage{*this, journalStorage, outboxStorage, peerStorage, modelCalls};
    TaskRecords taskRecords{*this, journalStorage, outboxStorage, waitStorage};
    const std::filesystem::path path;
    SecretStorage secretStorage{*this, path.parent_path()};
    MemoryStorage memoryStorage;
    BotLifecycleStorage botLifecycle{*this};
    AutomationStorage automationStorage;
    AttachmentStorage attachments{*this};
    ConversationContextStorage conversationContextStorage{*this, secretStorage, attachments};
    WorkflowStorage workflowStorage{*this,         taskRecords,       taskEvents,     journalStorage,
                                    secretStorage, conversationTurns, taskOperations, taskRouting};
    WebIdentityState webIdentity;
    InboxStorage inboxStorage{*this, secretStorage, taskRecords,    waitStorage,    journalStorage,
                              *this, modelCalls,    taskOperations, policyApprovals};
    GitStorage gitStorage{*this,
                          secretStorage,
                          inboxStorage,
                          taskRecords,
                          journalStorage,
                          webIdentity,
                          permissions,
                          conversationTurns,
                          botLifecycle,
                          policyApprovals,
                          sessionParticipationStorage};
    Policy permissions{*this};
    ChannelStorage channelStorage{*this, path, permissions, secretStorage, webIdentity, journalStorage, conversationTurns, policyApprovals};
    LocalMachineStorage localMachineStorage{*this, permissions, secretStorage, webIdentity, taskRecords};
    PolicyApprovals policyApprovals{*this,       secretStorage,     webIdentity,  taskRecords,    waitStorage, journalStorage,
                                    permissions, conversationTurns, botLifecycle, channelStorage, inboxStorage};
    SessionParticipationStorage sessionParticipationStorage{*this,
                                                            path,
                                                            secretStorage,
                                                            taskRecords,
                                                            webIdentity,
                                                            permissions,
                                                            conversationTurns,
                                                            conversationTools,
                                                            sessionMessageStorage,
                                                            botLifecycle,
                                                            taskOperations,
                                                            policyApprovals};
    SessionReadStorage sessionReadStorage{*this,          secretStorage,  sessionParticipationStorage, permissions, conversationTurns,
                                          channelStorage, policyApprovals};
    explicit Impl(std::filesystem::path path, MemorySeed, AutomationModelResolver);
};

}
