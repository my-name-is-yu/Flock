#pragma once

#include "attachment_storage.hpp"
#include "automation_storage.hpp"
#include "bot_lifecycle_storage.hpp"
#include "bot_settings_storage.hpp"
#include "bot_setup_storage.hpp"
#include "channel_connectors.hpp"
#include "channel_delivery_queue.hpp"
#include "channel_storage.hpp"
#include "conversation_context_storage.hpp"
#include "conversation_messages.hpp"
#include "conversation_tools.hpp"
#include "conversation_turns.hpp"
#include "turn_runner.hpp"
#include "database.hpp"
#include "git_storage.hpp"
#include "inbox_storage.hpp"
#include "journal_storage.hpp"
#include "local_machine_storage.hpp"
#include "memory_storage.hpp"
#include "model_calls.hpp"
#include "outbox_storage.hpp"
#include "policy_approvals.hpp"
#include "secret_storage.hpp"
#include "session_message_storage.hpp"
#include "session_participation_records.hpp"
#include "session_participation_storage.hpp"
#include "session_read_storage.hpp"
#include "session_send_receipts.hpp"
#include "task_events.hpp"
#include "task_operations.hpp"
#include "task_records.hpp"
#include "task_routing.hpp"
#include "task_run_reports.hpp"
#include "turn_results.hpp"
#include "wait_storage.hpp"
#include "web_identity_state.hpp"
#include "flock/bot_access.hpp"
#include "flock/policy.hpp"
#include "flock/secret_requests.hpp"
#include "flock/store.hpp"
#include "flock/store_options.hpp"

#include <filesystem>

namespace flockv0 {
struct Store::Impl {
    Store& store;
    StoreOptions options;
    Database database;
    const std::filesystem::path path;
    Policy permissions{store};
    BotAccess botAccess{store};
    SecretRequests secretRequests{store};
    ConversationMessages conversationMessages{database, permissions, webIdentity};
    ChannelDeliveryQueue channelDeliveries{database, permissions};
    SessionParticipationRecords sessionParticipations{database};
    TurnResults turnResults{database};
    SessionSendReceipts sessionSendReceipts{database, secretStorage, sessionParticipations, taskRecords, taskRunReports};
    ChannelConnectors channelConnectors{database, permissions, webIdentity};
    ChannelNotices channelNotices{database, channelConnectors, permissions, journalStorage, channelDeliveries};
    BotSettingsStorage botSettingsStorage{database, permissions, secretRequests, taskRouting};
    ConversationTurns conversationTurns{database,    attachments,          botLifecycle,      channelStorage,        gitStorage,
                                        permissions, journalStorage,       modelCalls,        secretStorage,         taskRecords,
                                        waitStorage, conversationMessages, channelDeliveries, sessionParticipations, channelConnectors,
                                        turnResults, sessionSendReceipts};
    TurnRunner turnRunner{database,
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
                          localMachineStorage,
                          memoryStorage,
                          modelCalls,
                          policyApprovals,
                          secretStorage,
                          sessionMessageStorage,
                          sessionParticipationStorage,
                          sessionReadStorage,
                          taskOperations,
                          taskRecords,
                          taskRouting,
                          waitStorage,
                          conversationMessages,
                          turnResults,
                          conversationTurns};
    ConversationTools conversationTools{database,      attachments,        conversationTurns, conversationMessages, permissions,
                                        path,          botSettingsStorage, modelCalls,        policyApprovals,      sessionReadStorage,
                                        secretStorage, turnResults};
    SessionMessageStorage sessionMessageStorage{database,
                                                botLifecycle,
                                                conversationTurns,
                                                conversationMessages,
                                                turnResults,
                                                gitStorage,
                                                permissions,
                                                path,
                                                journalStorage,
                                                outboxStorage,
                                                policyApprovals,
                                                secretStorage,
                                                sessionParticipations,
                                                sessionReadStorage,
                                                sessionSendReceipts,
                                                attachments};
    ModelCalls modelCalls{database, botLifecycle, permissions, secretStorage};
    TaskOperations taskOperations{database,   botLifecycle, permissions, journalStorage, outboxStorage, secretStorage,
                                  taskEvents, taskRecords,  taskRouting, waitStorage,    inboxDecisions};
    TaskRouting taskRouting{database, permissions, waitStorage, sessionReadStorage};
    TaskRunReports taskRunReports{database, policyApprovals, journalStorage, outboxStorage};
    OutboxStorage outboxStorage{database};
    JournalStorage journalStorage{database, channelDeliveries};
    TaskEvents taskEvents{database};
    WaitStorage waitStorage{database, journalStorage};
    TaskRecords taskRecords{database, journalStorage, outboxStorage, waitStorage};
    SecretStorage secretStorage{database, path.parent_path()};
    MemoryStorage memoryStorage{database, secretStorage, permissions, conversationMessages, std::move(options.memorySeed)};
    BotLifecycleStorage botLifecycle{database};
    AutomationStorage automationStorage{database, secretStorage, botLifecycle, permissions, taskOperations};
    AttachmentStorage attachments{database};
    ConversationContextStorage conversationContextStorage{database, secretStorage, attachments};
    WebIdentityState webIdentity;
    InboxDecisions inboxDecisions{database, secretStorage};
    InboxStorage inboxStorage{database,    secretStorage,  taskRecords,     waitStorage,
                              permissions, taskOperations, policyApprovals, inboxDecisions};
    GitStorage gitStorage{database,    secretStorage,        taskRecords,  journalStorage,  webIdentity,
                          permissions, conversationMessages, botLifecycle, policyApprovals, sessionParticipations};
    ChannelStorage channelStorage{database, path, permissions, conversationMessages, policyApprovals, channelConnectors, channelNotices};
    LocalMachineStorage localMachineStorage{database, permissions, webIdentity, taskRecords};
    PolicyApprovals policyApprovals{database,          secretStorage,  webIdentity,          taskRecords,
                                    waitStorage,       permissions,    conversationMessages, botLifecycle,
                                    channelConnectors, channelNotices, inboxDecisions};
    SessionParticipationStorage sessionParticipationStorage{database,           path,
                                                            secretStorage,      taskRecords,
                                                            webIdentity,        permissions,
                                                            conversationTurns,  conversationMessages,
                                                            turnResults,        sessionMessageStorage,
                                                            botLifecycle,       taskOperations,
                                                            policyApprovals,    sessionParticipations,
                                                            sessionSendReceipts};
    SessionReadStorage sessionReadStorage{database,          secretStorage,  sessionParticipations, permissions, conversationMessages,
                                          channelConnectors, policyApprovals};
    BotSetupStorage botSetup{store, database, secretStorage, std::move(options.saveBotSetup), std::move(options.botSetupCatalog)};
    Impl(Store&, std::filesystem::path path, StoreOptions);
};

}
