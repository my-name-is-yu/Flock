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
build/app/flock answer TASK_ID WAIT_ID "Use main"
```

For an offline demonstration, use a separate Flock home and start
`flock run --fixed`, with `--no-web` if the build omits Web support. A request
starting with `ask:` produces a demo Bot question, and replying lets the
fixture Session run to completion.

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
