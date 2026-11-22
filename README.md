# Flock v0

Flock is a local C++20 runtime for multiple Bots. Each Bot owns the requests it receives and delegates the work to T3 Sessions. The CLI and Web UI share the same SQLite database.

## Build

You need a C++20 compiler, CMake 3.24 or later, and development libraries for SQLite 3.35 or later, libcurl, and OpenSSL. Connecting to a live model requires Node.js 22 or later. Connection tests use Python 3.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Configure a Bot

The default state directory is `~/.config/flock`. Override it with the `FLOCK_HOME` environment variable or the CLI's `--home` option. Configure the Bot in `bots/release/config.json` under that directory.

```json
{
  "name": "Release",
  "modelProvider": "chatgpt",
  "model": "gpt-6.2-sol",
  "environmentId": "<T3 environment ID>",
  "projectId": "<T3 project ID>",
  "allowedBots": ["verifier"],
  "conversationEnvironments": {
    "cloud Mac": "<connected T3 environment ID>"
  },
  "runTimeoutSeconds": 1800,
  "modelCallLimit": 20,
  "session": {
    "runtimeMode": "approval-required"
  },
  "cron": [
    {
      "id": "release-notes",
      "schedule": "0 10 * * 1",
      "timezone": "Asia/Tokyo",
      "request": "Summarize the open items for the next release."
    }
  ]
}
```

In the same directory, use `SOUL.md` for the Bot's role and decision criteria and `MEMORY.md` for assumptions that carry across tasks. Set `session.model` and `session.providerInstanceId` to select the Session's model and provider. Use `session.workspaceStrategy` to specify its workspace.

Optional `conversationEnvironments` entries map names used in requests such as “Run the measurement on the cloud Mac” to T3 environments with access to the same `projectId`. Each target environment needs its own T3 connection and execution permissions. Use a configured name or canonical ID. If an alias matches a canonical ID, that spelling selects the canonical ID. The Bot asks in the conversation when the destination is unclear.

Optional `allowedBots` entries list the Bot directory names this Bot may contact. Permission is directional: a sender must allow the recipient. Recipients can return questions, results, or failures through the same exchange.

Optional `modelCallLimit` sets the initial allowance per request (20 by default). Each step of the Bot's conversation turn, the tasks it starts, and work or consultations passed to other Bots share that allowance. Failed attempts and retries count. Use **Approve one more model call** on the paused message or task to continue. See the [CLI guide](app/README.md#model-call-allowance) for details.

## Connect a live model and T3

```sh
build/app/flock model login
build/app/flock model models
build/app/flock t3 login <environment-id> <MCP-URL>
build/app/flock run
```

Approve each OAuth connection in your browser. In ChatGPT, authorize Flock to use your plan. In T3, choose permissions that cover Flock's Session settings. Open the Web UI at the local URL printed by the CLI. The running runtime continues to handle scheduled requests and monitor Sessions after you close the conversation.

Use the Bot's message box in the Web UI to send a new request, ask for progress, add instructions, or ask the Bot to stop something. You never need task numbers: refer to a request by what you asked for. If the Bot cannot tell which request you mean, it asks in the conversation, often with suggested replies you can tap. Additional instructions for an active task are saved for its next safe point, or the Bot can pass a note to the running Session right away. Asking the Bot to implement a completed result starts a new task linked to that result.

To try Flock without signing in, follow the [offline demo setup](app/README.md#offline-demo) to configure a Bot, grant launch access, and use `run --fixed`. Two Bots with different `projectId` values can work independently. Run `flock submit release 'ask:Which branch should I use?'` to try a request that waits for input before the first delegation.

```sh
build/app/flock --help
build/app/flock tasks
build/app/flock status <task-id>
build/app/flock history <task-id>
build/app/flock answer <task-id> <wait-id> 'main'
```

To check whether a live model applies different completion criteria, run `build/app/flock model acceptance <model-slug>` with an authorized ChatGPT connection. The command prints the decision and rationale for each trial as JSON Lines.

The Bot normally evaluates the Session's final report. To compare decisions based on the work history, run `run --session-read on_demand` to let the Bot read that history. The runtime saves up to three excerpts per event in SQLite, with a limit of 8 KiB per excerpt.

### Bot resource authorization

Grant the Bot the environment’s `launch` operation before starting its first
Session. A Bot can read its own Sessions. To share another Session, grant
`session:read` on the exact environment and thread. Use `Scope::sessionRead`
in C++, or the compact JSON pair `["environment-id","thread-id"]` as the resource
ID in settings. T3 must permit each read. Set `services` and `mcpServers` to arrays of
connection names for the conversation tools. Manage grants and protected input
from **Manage access and credentials** in Bot settings.

Adapters that perform resource operations should use the contracts in
`include/flock/access.hpp`. At the execution boundary, pass the authenticated
Session's environment and Run reference to `Access::executeSessionResource`,
together with the exact scope, the upstream permission check and the action.
Flock resolves the Run's owning task, checks current grants, and journals the
completed action. The execution callback receives the verified owning task ID for
resolving additional scoped resources. The transport must establish the calling Run's identity.
A peer's task and its Runs always resolve to the recipient Bot. Secret-consuming
adapters use `Access::useSecret` with the owning task and a named `SecretRef`.
External adapters must call these checks for each protected operation.

### Slack, iMessage, and local machines

Set `FLOCK_CHANNEL_BRIDGES_FILE` to a host-controlled JSON file before starting
`flock run`. Each channel workspace points to a provider bridge on
`127.0.0.1` and links provider sender IDs to Flock identities and Bots.

```json
{
  "slack": {
    "T123": {
      "bridgePort": 3410,
      "credentialEnv": "SLACK_BRIDGE_KEY",
      "senders": {"U123": {"personId": "alice", "botId": "release"}}
    }
  },
  "imessage": {
    "messages-on-mac": {
      "bridgePort": 3411,
      "credentialEnv": "IMESSAGE_BRIDGE_KEY",
      "senders": {"linked-sender": {"personId": "alice", "botId": "release"}}
    }
  },
  "artifactBaseUrl": "https://previews.example.com",
  "localMachines": {"laptop": "alice"}
}
```

Use a separate random bearer credential of at least 32 characters for each
workspace. Configure Flock Web identities and grant each linked person the
Bot's `decide` role. Grant the Bot `use` and `read` on the `service` resource
identified by `["slack","T123"]` or `["imessage","messages-on-mac"]`.

The provider bridge authenticates native events and retrieves attachment
bytes. It sends normalized JSON with `workspaceId`, `threadRef`,
`externalMessageId`, `senderId`, `text`, and optional `attachments` to
`POST /api/channels/slack/events` or `POST /api/channels/imessage/events`.
Each attachment has `name`, `mimeType`, and `contentBase64`. Use the workspace
bearer credential. Keep the same external message and thread identifiers for
retries. A Slack thread reference must identify both the channel and the thread.

Flock calls the bridge's `POST /deliver` with `idempotencyKey`, `origin`,
`operation`, and `payload`. `POST /reconcile` receives the same key and
origin. The bridge persists the delivery identity and returns JSON with
`state` set to `accepted`, `unknown`, `not_sent`, or `rejected`.
Include the provider's `externalMessageId` for an accepted post. Return
`not_sent` only with definitive evidence that a send has no provider effect.
This allows Flock to retry with the same key. The bridge should apply
`suggestions` from the reply payload using the channel's available controls
or plain text.

Expose only the artifact preview path at the optional HTTPS
`artifactBaseUrl` if recipients need it remotely. Published links contain
random access tokens and serve sandboxed HTML.

Local machine entries name the Flock host and its owner. The owner can review
requests under **Bot access settings → Local machine connections**. Each
approval lists exact absolute paths for `read_file` or `list_directory`
within one active Session. The host adapter opens paths without following
symlinks. Reusable execution environments are configured through Bot settings.
