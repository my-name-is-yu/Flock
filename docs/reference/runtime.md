# Runtime and Bot configuration

## Local state

Flock selects its state directory in this order: `--home DIRECTORY`,
`FLOCK_HOME`, then `~/.config/flock`. The CLI and Web listener use the runtime's
SQLite Store. Flock reopens databases with the current schema version (9).
Use a new state directory for a database from an earlier build. `--home`
lets you start this build alongside existing data.

Bot files live in `bots/<botId>/` under the Flock home, with the directory name
as the Bot ID. Each decision reads the current `SOUL.md` and the latest
[private memory](memory-and-automation.md#memory). Intake records
model, reasoning effort, and Session settings for the task.

## Bot configuration

See [Configure a Bot](../guide/bots.md) for the Web editor. Example
`bots/release/config.json` for settings managed in files:

```json
{
  "name": "Release",
  "modelProvider": "chatgpt",
  "model": "MODEL_SLUG_FROM_FLOCK_MODEL_MODELS",
  "reasoningEffort": "low",
  "environmentId": "CANONICAL_T3_ENVIRONMENT_ID",
  "projectId": "CANONICAL_T3_PROJECT_ID",
  "allowedBots": ["verifier"],
  "runTimeoutSeconds": 1800,
  "modelCallLimit": 20,
  "fixPullRequests": true,
  "session": {
    "runtimeMode": "approval-required"
  }
}
```

The model catalog reports available reasoning levels when ChatGPT publishes
them. Otherwise Flock offers `none`, `minimal`, `low`, `medium`, `high`,
`xhigh`, and `max`. Omitting `reasoningEffort` uses the model default.

The optional `session` object accepts `model`, `providerInstanceId`,
`runtimeMode`, and `workspaceStrategy`. Omitted settings use T3's defaults.
If T3 cannot select a model for the project, set `model` and
`providerInstanceId` together using a pair listed by T3's
`orchestrator_capabilities`. Runtime modes are `approval-required`,
`auto-accept-edits`, `auto`, and `full-access`.

| Workspace | `workspaceStrategy` |
| --- | --- |
| Project checkout | `{"type":"root"}` |
| New worktree | `{"type":"worktree","baseRef":"origin/main","startFromOrigin":true}`, with optional `branch` |
| Existing worktree | `{"type":"existing_worktree","worktreePath":"/path/to/worktree"}` |

See [configured schedules](memory-and-automation.md#configured-schedules),
[model call allowance](model-allowance.md), and
[PR notifications](github.md#automatic-follow-ups) for the corresponding
configuration fields.

## T3 destinations

`environmentId` and `projectId` use the canonical T3 IDs. They pin the
default for new Sessions and configured schedules. Flock can select the sole
eligible connected environment and its sole active project. When it needs
a project selection, it asks and remembers the answer for that Bot and
environment. A personal Bot with a model configured can search and start work
in an existing Session with both IDs omitted, subject to
[Session access](session-evidence.md#access-and-continuation).

`conversationEnvironments` maps names to environment IDs or objects with
`environmentId` and `projectId`. A name can select a destination for a request
such as “Run the measurement on the cloud Mac.” Each environment keeps its own
project selection. An alias can specify a project or use a pinned or
remembered project for that environment.

## Bot-to-Bot requests

`allowedBots` lists the recipient Bot IDs and defaults to an empty list.
Permission is directional. A recipient can return questions, results, or
failures through the same exchange. It needs its own allowance in
`allowedBots` to initiate a new exchange back to the sender. Consultations can
run without T3.

For delegated work, the recipient uses its own Session settings and
configured destination. Without a pinned environment, it uses the sender's
environment if the recipient can use it, or its sole eligible connected
environment.

## Conversation behavior

The Bot replies in its conversation, explains results and next steps, and
lists work it started or changed. When you provide additional instructions
for an active T3 task, Flock saves them for the next safe point. Asking the
Bot to implement a completed result starts a new task linked to the result.

Task decisions, including completion and requests for more work, record the
evidence and rationale in task history. Git changes record the rationale
with the operation. See [Bot tools](bot-tools.md) for routine calls.

## Pausing and resuming

Pause closes unfinished conversations and requests, refuses fresh messages,
and asks T3 to interrupt active Runs the Bot started. It preserves history
and persists across restarts. In a shared Session, delegated work can
continue if the Bot's reply already finished. External T3 connections
cannot interrupt those delegates individually.

See [automation during a pause](memory-and-automation.md#automation) and the
[Web response](http-api.md#bot-settings).

## CLI messages

`flock message` queues the message as the local owner and returns a receipt
with `messageId` and `status: "received"`. The running daemon processes the
message in the Bot's conversation. When it starts a task, `flock tasks` lists
that message ID in `sourceMessageId`. A fresh message to a paused Bot returns
**This Bot is paused.**

## ChatGPT connection

Each Flock home has one ChatGPT connection. The C++ runtime performs Flock's
OAuth flow and streams the public Responses endpoint using libcurl and
OpenSSL. It locks the profile during sign-in or refresh and releases the lock
before inference. ChatGPT credentials live in `credentials/chatgpt/` under the
Flock home. T3 credentials have their own directory.

See the official [sign-in guide][signin], [model requests][models], and
[function calling constraints][limits].

[signin]: https://developers.openai.com/siwc/token-sharing-open-source/sign-in
[models]: https://developers.openai.com/siwc/token-sharing-open-source/models-and-inference
[limits]: https://developers.openai.com/siwc/token-sharing-open-source/preview-limitations

## Model acceptance

After [sign-in](../guide/connections.md#connect-chatgpt), select a model slug
from the catalog and run the six-trial check.

```sh
build/app/flock model acceptance MODEL_SLUG > model-acceptance.jsonl
```

It emits one JSON Lines record per trial with the selected action, arguments,
and reason or error. Exit status is 0 only when all six trials choose their
expected action and provide a reason.
