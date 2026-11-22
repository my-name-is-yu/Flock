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
notes in `MEMORY.md`. Flock reloads those two files at each decision. Intake
records the selected model and Session settings for each task.

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
Add the IDs of Bots that this Bot may contact to `allowedBots`. It defaults
to an empty list. For the example above, set up `bots/verifier/` with its own
configuration and role. Release may send Verifier a question or request work,
and Verifier can reply within that request. Set `allowedBots` on Verifier
separately if it should also initiate new requests to Release. When a request
needs a Session, the receiving Bot uses its own configured route and settings.

Omit `cron` or set it to an empty array for a manual-only Bot. Cron uses five
numeric fields with wildcards, lists, ranges, and steps. In the weekday field,
both 0 and 7 mean Sunday. Schedules use the configured IANA timezone and
register each matching UTC minute during Runtime operation.

## Connect and run

```sh
build/app/flock model login
build/app/flock model status
build/app/flock model models
build/app/flock t3 login ENVIRONMENT_ID https://T3_HOST/mcp
build/app/flock run
```

Approve the two Flock connections in your browser. For ChatGPT sign-in, grant
permission to use the eligible ChatGPT plan. Copy a slug from `flock model
models` into each Bot configuration. The CLI supports a re-consent flow
through `flock model login --reconsent`. The active connection and a link to
manage plan usage appear in `flock model status`.

The foreground process prints the local Web URL. Use `flock run --no-web`
for headless operation. Once the daemon is running, submit work with the Web
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
configure the `release` Bot there. Replace `ENVIRONMENT_ID` with the value of
`environmentId` in its `config.json`. Grant launch access before submitting a
task, then start the demo runtime.

```sh
build/app/flock access grant release environment ENVIRONMENT_ID launch
build/app/flock run --fixed --no-web
```

In a second terminal using the same Flock home, submit a task.

```sh
build/app/flock submit release "Check the upcoming release"
```

Use `flock access list release` to see its grants and `flock access revoke
release environment ENVIRONMENT_ID launch` to remove this grant. A request
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
