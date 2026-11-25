# Try Flock offline

From the repository root, start the built executable with a separate Flock
home. The demo uses built-in model and T3 adapters.

```sh
demo_home=$(mktemp -d)
build/app/flock --home "$demo_home" run --fixed
```

1. Open the printed Web URL, choose **New Bot**, and create a Bot named
   **Release**.
2. Open the Bot's settings. Under **How it runs**, choose **Demo** as the
   model and enter `demo-env` for **Environment** and `demo-project` for
   **Project**. Save the settings.
3. Send “Check the upcoming release” in the Bot's conversation. The Bot
   replies and runs simulated work.
4. Send `ask:Which branch should I use?` to try a request that waits for an
   answer before delegation. Answer the question and watch the demo Session
   finish. To try independent work, set a different project ID on a second Bot.

## Run the demo from the CLI

To create the same Bot from files, put the following in
`bots/release/config.json` under the demo home. You can also use
`FLOCK_HOME` to select the home.

```json
{
  "name": "Release",
  "environmentId": "demo-env",
  "projectId": "demo-project"
}
```

Start the runtime and leave it running. Use `--no-web` for a headless demo,
or leave Web enabled if you want to [approve actions](flock-settings.md#permissions-and-approvals).

```sh
build/app/flock --home "$demo_home" run --fixed --no-web
```

In another terminal, set `demo_home` to the same directory and send a message.

```sh
build/app/flock --home "$demo_home" message release "Check the upcoming release"
build/app/flock --home "$demo_home" message release 'ask:Which branch should I use?'
```

Use the [CLI task commands](bots.md#use-the-cli) to check progress or answer
the question.
