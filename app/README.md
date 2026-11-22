# Flock CLI

Build from the repository root with CMake 3.24+ and a C++20 compiler. The
ChatGPT transport requires Node.js 22+ on `PATH`.

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
the latest private memory in SQLite. Intake records the selected model and
Session settings for each task.

Example `bots/release/config.json`

```json
{
  "name": "Release",
  "modelProvider": "chatgpt",
  "model": "MODEL_SLUG_FROM_FLOCK_MODEL_MODELS",
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

Use the exact environment and project IDs from T3. The optional `session`
object accepts `model`, `providerInstanceId`, `runtimeMode`, and
`workspaceStrategy`. Omitted Session settings use the T3 project's defaults.
Set `model` and `providerInstanceId` together. Runtime modes are
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
build/app/flock t3 connect ENVIRONMENT_ID https://T3_HOST/mcp
build/app/flock run
```

Approve the two Flock connections in your browser. For ChatGPT sign-in, grant
permission to use the eligible ChatGPT plan. Copy a slug from `flock model
models` into each Bot configuration. The CLI supports a re-consent flow
through `flock model login --reconsent`. The active connection and a link to
manage plan usage appear in `flock model status`.

The foreground process prints the local Web URL. Use `flock run --no-web`
for headless operation. T3 skills require the Web listener on the port saved by
`flock t3 connect`. Once the daemon is running, submit work with the Web
conversation or with CLI commands such as the following.

```sh
build/app/flock submit release "Check the upcoming release"
build/app/flock tasks
build/app/flock status TASK_ID
build/app/flock history TASK_ID
build/app/flock answer TASK_ID WAIT_ID REVISION_ID GENERATION "Use main"
```

## Offline demo

For an offline demonstration, set `FLOCK_HOME` to a separate directory and
configure the `release` Bot there. Start the demo runtime.

```sh
build/app/flock run --fixed --no-web
```

In a second terminal using the same Flock home, submit a task.

```sh
build/app/flock submit release "Check the upcoming release"
```

Full mode is the default. Manage installation-wide Never rules with
`flock rules list`, `flock rules add never ACTION TARGET` and
`flock rules remove never ACTION TARGET`. For example, target `demo-*` with
`environment.launch` to block launches into matching environments.
The local owner manages Bots created from the CLI or configuration. A person
who creates a Bot in Web owns it and can add shared members for conversation
and decisions.

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

The C++ Runtime owns the decision contract, state transitions, and T3 side
effects. The local Node transport performs the Flock OAuth flow and streams
the public Responses endpoint. It inherits a short-lived credential lock and
releases it before inference. Credentials live under
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
`modelCallLimit`. Calls in a parallel batch reserve available action slots
in provider order. An approval resumes the next pending action when a batch
pauses. The Bot receives results in provider order when the batch finishes.
