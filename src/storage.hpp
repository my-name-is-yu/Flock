#pragma once

#include "attachment_storage.hpp"
#include "automation_storage.hpp"
#include "bot_lifecycle_storage.hpp"
#include "bot_settings_storage.hpp"
#include "bot_setup_storage.hpp"
#include "channel_storage.hpp"
#include "conversation_context_storage.hpp"
#include "conversation_tools.hpp"
#include "conversation_turns.hpp"
#include "database.hpp"
#include "git_storage.hpp"
#include "inbox_storage.hpp"
#include "journal_storage.hpp"
#include "local_machine_storage.hpp"
#include "memory_storage.hpp"
#include "model_calls.hpp"
#include "outbox_storage.hpp"
#include "peer_storage.hpp"
#include "policy_approvals.hpp"
#include "secret_storage.hpp"
#include "session_message_storage.hpp"
#include "session_participation_storage.hpp"
#include "session_read_storage.hpp"
#include "task_events.hpp"
#include "task_operations.hpp"
#include "task_records.hpp"
#include "task_routing.hpp"
#include "task_run_reports.hpp"
#include "wait_storage.hpp"
#include "web_identity_state.hpp"
#include "workflow_storage.hpp"
#include "flock/bot_access.hpp"
#include "flock/policy.hpp"
#include "flock/secret_requests.hpp"
#include "flock/store.hpp"

#include <filesystem>

namespace flockv0 {
struct Store::Impl {
    Database database;
    const std::filesystem::path path;
    Policy permissions;
    BotAccess botAccess;
    SecretRequests secretRequests;
    BotSettingsStorage botSettingsStorage{database, permissions, secretRequests, sessionParticipationStorage, taskRouting};
    ConversationStorage conversationTurns{database,
                                          attachments,
                                          automationStorage,
                                          botLifecycle,
                                          botAccess,
                                          botSettingsStorage,
                                          botSetup,
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
    ConversationTools conversationTools{database,           attachments, conversationTurns, permissions,        path,
                                        botSettingsStorage, modelCalls,  policyApprovals,   sessionReadStorage, secretStorage};
    SessionMessageStorage sessionMessageStorage{database,
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
    ModelCalls modelCalls{database, botLifecycle, permissions, secretStorage};
    TaskOperations taskOperations{database,      automationStorage, botLifecycle, permissions, journalStorage, outboxStorage, peerStorage,
                                  secretStorage, taskEvents,        taskRecords,  taskRouting, waitStorage,    inboxStorage};
    TaskRouting taskRouting{database, permissions, peerStorage, waitStorage, sessionReadStorage, sessionParticipationStorage};
    TaskRunReports taskRunReports{database, policyApprovals, journalStorage, outboxStorage};
    OutboxStorage outboxStorage{database};
    PeerStorage peerStorage{database};
    JournalStorage journalStorage{database, channelStorage};
    TaskEvents taskEvents{database};
    WaitStorage waitStorage{database, journalStorage, outboxStorage, peerStorage};
    TaskRecords taskRecords{database, journalStorage, outboxStorage, waitStorage};
    SecretStorage secretStorage{database, path.parent_path()};
    MemoryStorage memoryStorage;
    BotLifecycleStorage botLifecycle{database};
    AutomationStorage automationStorage;
    AttachmentStorage attachments{database};
    ConversationContextStorage conversationContextStorage{database, secretStorage, attachments};
    WorkflowStorage workflowStorage{database,      taskRecords,       taskEvents,     journalStorage,
                                    secretStorage, conversationTurns, taskOperations, taskRouting};
    WebIdentityState webIdentity;
    InboxStorage inboxStorage{database,       secretStorage, taskRecords,    waitStorage,
                              journalStorage, permissions,   taskOperations, policyApprovals};
    GitStorage gitStorage{database,
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
    ChannelStorage channelStorage{database, path, permissions, webIdentity, journalStorage, conversationTurns, policyApprovals};
    LocalMachineStorage localMachineStorage{database, permissions, webIdentity, taskRecords};
    PolicyApprovals policyApprovals{database,    secretStorage,     webIdentity,  taskRecords,    waitStorage,
                                    permissions, conversationTurns, botLifecycle, channelStorage, inboxStorage};
    SessionParticipationStorage sessionParticipationStorage{database,
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
    SessionReadStorage sessionReadStorage{database,       secretStorage,  sessionParticipationStorage, permissions, conversationTurns,
                                          channelStorage, policyApprovals};
    BotSetupStorage botSetup;
    Impl(Store&, std::filesystem::path path, StoreOptions);
};

}
