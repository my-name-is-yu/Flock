# Flock v0

Flock is a local C++20 runtime for multiple Bots. Each Bot owns the requests it receives and delegates the work to T3 Sessions. The CLI and Web UI share the same SQLite database.

## Build

You need a C++20 compiler, CMake 3.24 or later, and the SQLite, libcurl, and OpenSSL development libraries. Connecting to a live model requires Node.js 22 or later. Connection tests use Python 3.

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
  "conversationEnvironments": {
    "cloud Mac": "<connected T3 environment ID>"
  },
  "runTimeoutSeconds": 1800,
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

Optional `conversationEnvironments` entries map names used in requests such as “Run the measurement on the cloud Mac” to T3 environments with access to the same `projectId`. Each target environment needs its own T3 connection and execution permissions. Use a configured name or canonical ID. An ambiguous destination is confirmed before work starts.

## Connect a live model and T3

```sh
build/app/flock model login
build/app/flock model models
build/app/flock t3 login <environment-id> <MCP-URL>
build/app/flock run
```

Approve each OAuth connection in your browser. In ChatGPT, authorize Flock to use your plan. In T3, choose permissions that cover Flock's Session settings. Open the Web UI at the local URL printed by the CLI. The running runtime continues to handle scheduled requests and monitor Sessions after you close the conversation.

Use the Bot's message box in the Web UI to send a new request, ask for progress, or add instructions to a task. If the Bot cannot identify the task, it asks you to specify it by name. The runtime saves additional instructions for an active task before delivering them to its Session. Asking the Bot to implement a completed result starts a new task linked to that result.

To try Flock without signing in, configure a Bot in a test state directory and use `run --fixed`. Two Bots with different `projectId` values can work independently. Run `flock submit release 'ask:Which branch should I use?'` to try a request that waits for input before the first delegation.

```sh
build/app/flock --help
build/app/flock tasks
build/app/flock status <task-id>
build/app/flock history <task-id>
build/app/flock answer <task-id> <wait-id> 'main'
```

To check whether a live model applies different completion criteria, run `build/app/flock model acceptance <model-slug>` with an authorized ChatGPT connection. The command prints the decision and rationale for each trial as JSON Lines.

The Bot normally evaluates the Session's final report. To compare decisions based on the work history, run `run --session-read on_demand` to let the Bot read that history. The runtime saves up to three excerpts per event in SQLite, with a limit of 8 KiB per excerpt.
