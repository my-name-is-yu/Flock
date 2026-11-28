#pragma once

// Every environment variable Flock reads.
namespace flockv0::environment {
inline constexpr auto home = "FLOCK_HOME";
inline constexpr auto webIdentitiesFile = "FLOCK_WEB_IDENTITIES_FILE";
inline constexpr auto githubToken = "FLOCK_GITHUB_TOKEN";
inline constexpr auto githubWebhookSecret = "FLOCK_GITHUB_WEBHOOK_SECRET";
inline constexpr auto channelBridgesFile = "FLOCK_CHANNEL_BRIDGES_FILE";
inline constexpr auto webSearchKey = "FLOCK_WEB_SEARCH_KEY";
inline constexpr auto codexConfig = "FLOCK_CODEX_CONFIG";
inline constexpr auto codexCli = "FLOCK_CODEX_CLI";
inline constexpr auto claudeConfig = "FLOCK_CLAUDE_CONFIG";
inline constexpr auto claudeCli = "FLOCK_CLAUDE_CLI";
inline constexpr auto t3Home = "T3CODE_HOME";
inline constexpr auto recordT3 = "FLOCK_RECORD_T3";
inline constexpr auto recordChatGPT = "FLOCK_RECORD_CHATGPT";
inline constexpr auto recordChatGPTModel = "FLOCK_RECORD_CHATGPT_MODEL";
inline constexpr auto recordCommit = "FLOCK_RECORD_COMMIT";
}
