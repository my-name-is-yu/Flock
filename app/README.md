# Flock CLI

Build from the repository root with CMake 3.24+ and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
build/app/flock --help
```

Flock selects its state directory with `--home DIRECTORY`, then `FLOCK_HOME`,
then `~/.config/flock`. Set up each Bot in `bots/<botId>/` under that directory.
Place its role and decision criteria in `SOUL.md` and its task-independent
starting notes in `MEMORY.md`. Each decision reads the current `SOUL.md` and
the latest private memory in SQLite. Intake records the selected model,
reasoning effort, and Session settings for each task.

Example `bots/release/config.json`

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
  "session": {
    "runtimeMode": "approval-required"
  },
  "cron": [
    {
      "id": "weekday-check",
      "schedule": "0 9 * * 1-5",
      "timezone": "Asia/Tokyo",
      "request": "Check whether the release is ready and report the remaining work."
    }
  ]
}
```

Use the exact environment and project IDs from T3 for the Bot's default destination for
new Sessions and for configured schedules. A personal Bot with a model configured
can search connected T3 and start work in an existing Session with those IDs omitted.
The optional `session`
object accepts `model`, `providerInstanceId`, `runtimeMode`, and
`workspaceStrategy`. Omitted Session settings use T3's defaults. If T3 cannot
select a model for the project, set `model` and `providerInstanceId` together
using a pair listed by T3's `orchestrator_capabilities`. Runtime modes are
`approval-required`, `auto-accept-edits`, `auto`, and `full-access`.
For the project checkout, use `{"type":"root"}`. For a new worktree, use
`{"type":"worktree","baseRef":"origin/main","startFromOrigin":true}` and
optionally set `branch`. To use an existing worktree, set `type` to
`existing_worktree` and specify `worktreePath`.

If a task is waiting for a Session settings correction, update its Bot's
`config.json` and reconcile the task to retry the unsent action.

Add the IDs of Bots that this Bot may contact to `allowedBots`. It defaults
to an empty list. For the example above, set up `bots/verifier/` with its own
configuration and role. Release may send Verifier a question or request work,
and Verifier can reply within that request. Set `allowedBots` on Verifier
separately if it should also initiate new requests to Release. When a request
needs a Session, the receiving Bot uses its own configured route and settings.

Flock syncs configured `cron` entries into SQLite when Runtime starts. Keep each
entry's `id` stable to preserve its next occurrence. Omit `cron` or use an empty
array to deactivate configured schedules. Restoring an entry resumes it on its
next scheduled occurrence. A schedule canceled through Flock stays canceled
across syncs. Cron accepts five numeric fields with wildcards, lists, ranges,
and steps. In the weekday field, 0 and 7 both mean Sunday.

A missed reminder fires once on restart. Missed recurring occurrences produce
one task with their first and last scheduled times and occurrence count, then
resume according to the configured IANA timezone. Scheduling supports dates
through 2199.

Ask a Bot in its conversation to remember a convention, manage a schedule or
reminder, or follow up on a webhook. Flock imports `MEMORY.md` on the first
private memory access. SQLite supplies private memory for conversation and task
decisions from then on. Use the memory tools or Web API to edit it. Shared
playbooks and text documents use an explicit document ID and follow the installation policy
for the requested `shared_memory` operation.

## Connect and run

```sh
build/app/flock model login
build/app/flock model status
build/app/flock model models
build/app/flock t3 connect
build/app/flock run
```

Approve the two Flock connections in your browser. For ChatGPT sign-in, grant
permission to use the eligible ChatGPT plan and check for `planUsage: true` in
`flock model status`. Copy a slug from `flock model models` into each Bot
configuration. Use `flock model login --reconsent` if plan usage is missing.
`reasoningEffort` sets the effort for this Bot's ChatGPT decisions. Omit it to use
the model default. `flock model models` shows `reasoningEfforts` when ChatGPT
lists the supported levels. Otherwise Flock offers `none`, `minimal`, `low`,
`medium`, `high`, `xhigh`, and `max`. If ChatGPT rejects the chosen effort,
select another level or **Model default** in Bot settings.
For T3, approve Operate access. `flock t3 connect` discovers the running local
T3 server. To connect another machine, pass its MCP URL as the argument. When
authorization expires, use `flock t3 reconnect ENVIRONMENT` to approve again.
If Flock pauses a launch because of the installation policy, check the active
mode and rules in **Flock settings**.

The foreground process prints the local Web URL. Use `flock run --no-web`
for headless operation. T3 skills require the Web listener on the port saved by
`flock t3 connect`. Send a Bot message from the Web conversation or from the CLI.

```sh
build/app/flock message release "Check the upcoming release"
build/app/flock tasks
build/app/flock status TASK_ID
build/app/flock history TASK_ID
build/app/flock answer TASK_ID WAIT_ID REVISION_ID GENERATION "Use main"
```

`flock message` queues the message as the local owner and returns a receipt
with `messageId` and a status of `received`. The running daemon processes the
message in the Bot's conversation. If the Bot starts a Task, `flock tasks`
shows the same message ID in the Task's `sourceMessageId` field.

## Offline demo

For an offline demonstration, set `FLOCK_HOME` to a separate directory and
configure the `release` Bot there. Start the demo runtime.

```sh
build/app/flock run --fixed --no-web
```

In a second terminal using the same Flock home, send the Bot a message.

```sh
build/app/flock message release "Check the upcoming release"
```

Full mode is the default. Use `flock mode` to show the current mode and
`flock mode supervised` or `flock mode full` to change it. Use the Web UI to
confirm actions that require policy approval.

Use `flock rules list` and `flock rules add|remove allow|ask|never ACTION TARGET`
to manage installation rules. Quote glob targets, such as `'demo-*'` for
`environment.launch`. Never wins in both modes. Supervised checks Ask first
before Allow and caps each of those rule kinds at 20. A rule saved from a
card replaces the oldest Allow at capacity.

See the [installation policy table](../README.md#installation-policy) for
which actions ask. The requester and installation roots can decide cards.
Background cards go to the schedule's Bot owner or T3 connector person
and expire after 30 minutes. An expired card can save an Allow rule for
future work. Direct requests stay open. The local owner manages Bots
created through the CLI or configuration. Web Bot creators manage shared
members.

A request
starting with `ask:` produces a demo Bot question. Answering it lets the
fixture Session run to completion.

## Model call allowance

Set `modelCallLimit` in a Bot's `config.json` to a positive integer. The
default is 20. Flock takes the limit from the originating request's saved
settings. Each step of the Bot's conversation turn, the decisions of the tasks
the turn starts, and their Bot exchanges consume attempts from the same
allowance. Every message gets its own allowance.

Flock saves each attempt in SQLite before invoking the model. Failed attempts,
retries, and in-flight attempts at restart count. The `model_attempts` table
records usage fields when available. Missing fields are unknown.

When the allowance runs out, choose **Approve one more model call** on the
paused conversation message or task. For a task, `flock approve TASK WAIT REVISION GENERATION`
uses the wait ID, current revision (or `0` before the first revision), and generation from `flock status TASK`. If another Bot's task is paused, open that Bot's
waiting item. Each approval grants one attempt. The additional Run
allowance remains separate. T3 observation, reconciliation, and calls made
inside T3 or an external provider are outside the local model call allowance.

## Six-trial model acceptance

After ChatGPT sign-in, run the following command with a slug from the model
catalog. It writes one JSON record per trial with the chosen action, arguments,
and reason, or an error. All six must match their expected actions for exit
status 0.

```sh
build/app/flock model acceptance MODEL_SLUG > model-acceptance.jsonl
```

The C++ runtime performs the Flock OAuth flow and streams the public Responses
endpoint using libcurl and OpenSSL. It locks the profile while signing in or
refreshing credentials and releases the lock before inference. Credentials live under
`credentials/chatgpt/`; T3 credentials have their own directory. Each Flock
home has one ChatGPT connection. See the official [sign-in guide][signin],
[model requests][models], and [function calling constraints][limits].

[signin]: https://developers.openai.com/siwc/token-sharing-open-source/sign-in
[models]: https://developers.openai.com/siwc/token-sharing-open-source/models-and-inference
[limits]: https://developers.openai.com/siwc/token-sharing-open-source/preview-limitations

## Bot tools

Conversation turns can use the Bot's workspace under `<home>/workspaces/<botId>`.
Policy actions `workspace.read`, `workspace.write` and `workspace.exec` target
the Bot ID. Commands require read and exec and receive write access when the
policy allows it. macOS uses the system sandbox. Linux requires
`/usr/bin/bwrap` with unprivileged namespaces, FD-based bind support, and
seccomp filtering; Linux builds also require the libseccomp development
library. Existing single-command `argv` calls retain their behavior. On
macOS and Linux, `workspace_run_command` also accepts a structured pipeline:

```json
{"pipeline":[["cat","allowed.txt"],["wc","-c"]]}
```

A pipeline contains 1–4 argv arrays. Its executable in each stage must resolve
to system `/bin` or `/usr/bin` and have one of these names: `cat`, `cut`,
`echo`, `grep`, `head`, `printf`, `sleep`, `sort`, `tail`, `tee`, `tr`,
`uniq`, `wc`, or `yes`. Shell syntax is not parsed; metacharacters remain
literal argv data. Scripts, shells, interpreters, workspace executables, and
other helpers are rejected before any stage starts; use T3 Sessions for them,
regular builds, and tests. This allowlist is a partial supported subset, not a
security boundary: macOS retains its fork-denying profile, and Linux applies
a libseccomp filter through Bubblewrap to prevent stage process/thread
creation. Filter-generation or kernel/Bubblewrap refusal fails closed.

Commands have a shared 10-second wall limit and combined 8 KiB stdout/stderr
capture, with one active command per Bot and noninteractive stdin. Each
pipeline payload stage receives a CPU limit of `max(1, floor(5/N))` seconds;
core dumps are disabled, file size is limited to 1 MiB, and open files to 64.
These are per-process limits and do not guarantee a strict aggregate CPU
ceiling for trusted Bubblewrap monitor overhead. A running command whose
outcome cannot be confirmed after restart or supervisor failure is recorded
as `unknown_after_restart` and is not replayed automatically.

Web fetch requires `web/<hostname>/fetch` and accepts public HTTPS text
responses. Web search requires `web/public/search` and the host environment
variable `FLOCK_WEB_SEARCH_KEY` for Brave Search. After reading web evidence,
the Bot can keep reading and reply. Actions require a new user message.

Set optional skill and MCP targets in `<home>/bot-tools.json`.
Relative paths are resolved against the Flock home.

```json
{
  "skills": {"writing": "/path/to/skills/writing"},
  "mcpServers": {
    "knowledge": {"url": "https://example.com/mcp", "tokenEnv": "KNOWLEDGE_MCP_TOKEN"}
  }
}
```

Skill actions are `skill.list` for the catalog and `skill.read` for individual
skills. MCP uses Streamable HTTP with `mcp.discover`, `mcp.invoke` and
`mcp.read_resource` actions targeting the configured server name. `tokenEnv`
is optional and points to a
host-managed bearer token. Discovery lists one collection per call and returns
`nextCursor` when another page is available.

Each authorized Bot tool action counts against the request's shared
`modelCallLimit`. ChatGPT can propose several independent reads in one conversation
response when at least two batchable tools are offered. Conversation batches can
read Session search and events, environment readiness, tasks and task results,
workflow status, schedules, secret names, connections, saved tool results, and
memory and revision history. Store reads run under the Store mutex, and T3 reads
can overlap. Flock schedules conflicting Bot tools in order and saves results
while individual calls wait for approval. The Bot receives results in provider
order, then can reply or propose an action on its own. Git reads and workspace
command launches must be requested on their own. Use the command status to check
completion before reading files the command produces.

Run `ctest --test-dir build -R '^flock_tool_batch_measure$' -V` to compare
single-call and batch timings, model attempts, budget units, and budget pauses
using deterministic fake providers.
