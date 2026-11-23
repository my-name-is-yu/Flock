# Flock v0

Flock is a local C++20 runtime for multiple Bots. Each Bot owns the requests it receives and delegates the work to T3 Sessions. The CLI and Web UI share the same SQLite database.

Flock reopens databases with the current schema version (6). Databases from earlier builds require a new state directory. Use `--home` to start the current build alongside your existing data.

## Build

You need a C++20 compiler, CMake 3.24 or later, and development libraries for SQLite 3.35 or later, libcurl, OpenSSL, libpng 1.6 or later, and libjpeg. Connecting to a live model requires Node.js 22 or later. Connection tests use Python 3.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For one-off verification, run `./scripts/verify.sh`. It creates a temporary
build directory and removes it when the command exits. Use `--keep-build` to
keep the directory for debugging. Set `CMAKE_BUILD_PARALLEL_LEVEL` to control
build concurrency.

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

In the same directory, use `SOUL.md` for the Bot's role and decision criteria and `MEMORY.md` to seed its versioned private memory. Set `session.model` and `session.providerInstanceId` to select the Session's model and provider. Use `session.workspaceStrategy` to specify its workspace.

Optional `conversationEnvironments` entries map names used in requests such as “Run the measurement on the cloud Mac” to T3 environments with access to the same `projectId`. Each target environment needs its own T3 connection and execution permissions. Use a configured name or canonical ID. If an alias matches a canonical ID, that spelling selects the canonical ID. The Bot asks in the conversation when the destination is unclear.

Optional `allowedBots` entries list the Bot directory names this Bot may contact. Permission is directional: a sender must allow the recipient. Recipients can return questions, results, or failures through the same exchange.

Optional `modelCallLimit` sets the initial allowance per request (20 by default). Each step of the Bot's conversation turn, the tasks it starts, and work or consultations passed to other Bots share that allowance. Failed attempts and retries count. Use **Approve one more model call** on the paused message or task to continue. See the [CLI guide](app/README.md#model-call-allowance) for details.

## Connect a live model and T3

```sh
build/app/flock model login
build/app/flock model status
build/app/flock model models
build/app/flock t3 connect <environment-id> <MCP-URL>
build/app/flock run
```

Approve each OAuth connection in your browser. In ChatGPT, authorize Flock to use your plan and check that `model status` shows `planUsage: true`. In T3, approve Operate access and choose permissions that cover Flock's Session settings. Use the connected environment ID in the Bot configuration. Open the Web UI at the local URL printed by the CLI. The running runtime continues to handle scheduled requests and monitor Sessions after you close the conversation.

In a T3 Session, ask for a Bot by name or select `/skill:flock-<bot>` or
`$flock-<bot>` in the provider's skill menu. The Bot replies in the same Session.
Open **Flock settings** to see the connected environments and saved loopback port. Use `--port PORT` to change the port.
See [Ask a Bot from a T3 Session](web/README.md#ask-a-bot-from-a-t3-session) for connection details.

Use the Bot's message box in the Web UI to send a new request, ask for progress, add instructions, or ask the Bot to stop something. You never need task numbers: refer to a request by what you asked for. If the Bot cannot tell which request you mean, it asks in the conversation, often with suggested replies you can tap. With T3, additional instructions for an active task are saved for its next safe point. Asking the Bot to implement a completed result starts a new task linked to that result.

To try Flock without signing in, follow the [offline demo setup](app/README.md#offline-demo) to configure a Bot and use `run --fixed`. Two Bots with different `projectId` values can work independently. Run `flock message release 'ask:Which branch should I use?'` to try a request that waits for input before the first delegation.

```sh
build/app/flock --help
build/app/flock tasks
build/app/flock status <task-id>
build/app/flock history <task-id>
build/app/flock answer <task-id> <wait-id> <revision-id> <generation> 'main'
```

To check whether a live model applies different completion criteria, run `build/app/flock model acceptance <model-slug>` with an authorized ChatGPT connection. The command prints the decision and rationale for each trial as JSON Lines.

The Bot normally evaluates the Session's final report. It can retry an unavailable final report once Flock has verified which Run produced it. To compare decisions based on the work history, run `run --session-read on_demand` to let the Bot read that history. The runtime allows up to three evidence reads per event and saves history excerpts of up to 8 KiB each in SQLite.

### Installation policy

The installation starts in **Full**. Bots can use configured resources unless
a Never rule matches. Switch to **Supervised** to approve actions with external
effects as they come up. Installation roots manage the mode and rules in **Flock settings**.
Workspace isolation, upstream credentials, and Bot
ownership apply in both modes.

| Action | Supervised default |
| --- | --- |
| Repository writes, PR changes, reviews, and issue or PR comments | Ask |
| Channel posts outside the asking conversation, published artifacts, and email sends | Ask |
| T3 Session launch or a send outside the originating Thread | Ask |
| Bot creation or rename, schedule, reminder, or trigger changes | Ask |
| Distinct destructive operations or secret use at a new destination | Ask |
| MCP tool calls | Allow when discovery returns `annotations.readOnlyHint: true` for the tool. Hints apply to the configured server and exact tool for two minutes. Ask otherwise |
| Reads, workspace work, and replies, reactions, edits, and attachments in the originating conversation | Allow |

Targets match exactly or with a `*` glob. **Never** applies in both modes.
Supervised evaluates **Ask first** ahead of **Allow**, then uses the defaults
above. The installation can have up to 20 Allow rules and 20 Ask-first rules.
When a card adds a 21st Allow rule, Flock replaces the oldest Allow rule.

```sh
flock mode
flock mode supervised
flock rules list
flock rules add ask repository.pr 'github.com/acme/*'
flock rules remove ask repository.pr 'github.com/acme/*'
```

The requester's cards appear in the Decision Inbox and inline with their
originating conversation when there is one. The requester or an installation
administrator can choose **Allow once** or **Deny**. Deny blocks another attempt
until a new message from a person. An installation administrator can choose
**Allow for this Flock** to approve the action and save an exact rule for all
Bots in the installation. Work requested by a person stays open. Background
approvals expire after 30 minutes. An administrator can choose **Allow next time
for this Flock** on an expired card to save a rule for future work. A
card-requiring action without an eligible requester is recorded and denied.

A message from a linked Flock person can start work that needs approval. Flock
posts a waiting notice when replies are allowed in the channel. The requester or installation root
answers in Flock. Channel messages cannot answer approvals or save rules.

Session read and T3 send targets use compact JSON
`["environment-id","thread-id"]`. Secret values stay inside adapters and
are excluded from card previews.

In a personal Bot conversation, the connection owner or installation root can
search readable Sessions across connected T3 projects. Shared Bot conversations
use their configured project scope. External channels search Sessions recorded
for the same conversation.

To resume work in a Session, ask your Bot to find it and give it an instruction,
such as “Find the login investigation and add a test in that Session.” Flock
sends the instruction to that Session and tracks the new Run, whether the Session
was started in T3 or by a Bot. If the Session is busy, T3 queues the work.
The normal T3 send policy and T3's Operate permission apply.

Session search scans up to 64 candidates per page with a shared two-second
deadline for candidate reads. A cursor after a matched Session first rechecks
that Session's readability with its own two-second deadline. Follow `nextCursor`
with the same filters even when the page contains no matches. If a timeout occurs
before the scan advances, retry with the same cursor.

Adapters that perform resource operations should use the contracts in
`include/flock/policy.hpp`. At the execution boundary, pass the authenticated
Session's environment and Run reference to `Policy::executeSessionResource`,
together with the exact scope, the upstream permission check and the action.
Flock resolves the Run's owning task, checks current policy, and journals the
completed action. The execution callback receives the verified owning task ID for
resolving additional resource operations. The transport must establish the calling Run's identity.
A peer's task and its Runs always resolve to the recipient Bot. Secret-consuming
adapters use `Policy::useSecret` with the owning task, named `SecretRef`,
canonical destination, and stable operation key. An adapter that receives a
pending-approval outcome can retry the same key after approval. Flock passes
the secret through the callback only when that operation is admitted.

### GitHub repositories and PR notifications

Build with libcurl 7.85 or newer. Set `FLOCK_GITHUB_TOKEN` in the daemon
environment to connect GitHub. Give the token access to the intended repositories and operations. Repository
rules address lowercase `github.com/owner/repo` targets. Use `repository.read`
for reads, `repository.pr` for PR changes, `repository.review` for reviews, and
`repository.issues` for issue changes. Opening a PR from a different head
repository also requires read access to that repository.

Set `FLOCK_GITHUB_WEBHOOK_SECRET` to the secret configured for a GitHub JSON
webhook. Forward signed deliveries to `POST /api/git/webhooks/github.com` on the
Flock loopback listener, preserving the body and GitHub signature and delivery
headers. Subscribe to pull request events to track changes to watched branches.
Also subscribe to reviews, comments, checks, workflow jobs, and code scanning
as needed. Events with a matching watch go to the owning Bot, which needs
`read` on the repository. A workflow job event without a PR number is matched
by its commit SHA using the current watch and recorded PR synchronization events.

A lost PR creation response leaves a durable operation. Asking the Bot to retry
with the same exact repository and branch pair checks for the original PR
across all states. If its identity remains unresolved, the Decision Inbox shows
a reconciliation item. Link a verified candidate or reject the operation.
A definitive host rejection allows a fresh request. For a replacement after a
confirmed PR closes, ask the Bot to open a new PR explicitly.

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
  "localMachines": {"laptop": "alice"},
  "sessionBridgeCredentialEnv": "FLOCK_SESSION_BRIDGE_KEY"
}
```

Use a separate random bearer credential of at least 32 characters for each
workspace. Configure Flock Web identities and share the Bot with its linked people. Channel rules
use `channel.use`, `channel.read` and `channel.post` with targets such as
`["slack","T123"]` or `["imessage","messages-on-mac"]`.

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
or plain text. When a user selects a posted suggestion, including a task
question choice, send its label as `text` and the post's provider message ID
as `replyToExternalMessageId`. Reuse the target and choice event ID on retries.

To publish HTML artifacts in a channel, set an HTTPS `artifactBaseUrl`
and proxy only its artifact preview path to Flock. Published links contain
random access tokens, honor current workspace permission, and serve sandboxed HTML.

Local machine entries name the Flock host and its owner. The owner can review
requests under **Sharing and connections → Local machine connections**. Open
approval links in a browser on the Flock host, where Web listens on loopback. Each
approval lists exact absolute paths for `read_file` or `list_directory`
within one active Session. A trusted Session bridge authenticates the caller
and sends its verified `environmentId`, `threadId` and `runId` with
`machineId`, `operation` and `path` to loopback
`POST /api/channels/local-operations`. The request uses a separate bearer
credential from `sessionBridgeCredentialEnv`, held by the bridge. C++ adapters
can use `Store::runLocalMachineOperation` with the same authenticated Run.
The host opens paths without following symlinks. Reusable execution
environments are configured through Bot settings.

## Bot workspace commands

`workspace_run_command` keeps the existing single-command `argv` form and also
accepts a structured pipeline on macOS and Linux:

```json
{"pipeline":[["cat","allowed.txt"],["wc","-c"]]}
```

Pass argv arrays, not shell text: metacharacters are literal argument data.
Pipelines contain 1–4 stages using only system `/bin` or `/usr/bin` executables
named `cat`, `cut`, `echo`, `grep`, `head`, `printf`, `sleep`, `sort`, `tail`,
`tee`, `tr`, `uniq`, `wc`, or `yes`. Scripts, shells, interpreters, workspace
executables, and other helpers are rejected before launch; use a T3 Session
for them, builds, and tests. This allowlist is a partial supported subset, not
a security boundary; the platform sandbox remains the security boundary.

Commands use noninteractive stdin, one active command per Bot, a shared
10-second wall limit, and a combined 8 KiB stdout/stderr capture. Each pipeline
payload stage receives a CPU limit of `max(1, floor(5/N))` seconds, with core
dumps disabled, a 1 MiB file-size limit, and 64 open files. Linux builds
require libseccomp and `/usr/bin/bwrap` with the requested seccomp filtering
support; filtering errors fail closed. macOS keeps its fork-denying sandbox.
An outcome that cannot be confirmed after restart or supervisor failure is
`unknown_after_restart` and is never replayed automatically.
These are per-process limits; they do not guarantee a strict aggregate CPU
ceiling for trusted Bubblewrap monitor overhead.
