# Model call allowance

The **Steps per request** setting in a Bot's **How it runs** settings controls
the initial allowance for future requests. On the host, the equivalent
`config.json` field is `modelCallLimit`, a positive integer up to
2,147,483,647 that defaults to 20.

Flock saves the initial allowance with the originating request. The Bot's
conversation turns for that request, decisions on work those turns start, and
work or consultations handed to other Bots draw from the same allowance.
New requests receive their own allowance. T3 observation, reconciliation,
and calls inside T3 or an external provider fall outside this accounting.

## Accounting

Flock records an attempt in SQLite before invoking the model. Failed attempts,
retries, and attempts already in flight at restart count. Each authorized
Bot tool action also consumes a step, including each such action in a batch.
The `model_attempts` table holds reported usage fields, including
`input_tokens_details.cached_tokens`. A zero count means no input tokens were
reused. An absent count means usage is unknown.

ChatGPT requests sort tools by name. The input puts the Bot's role, private
memory, earlier conversation history, task state, evidence, and feedback
ahead of the current event and its tool results. This extends the prefix
that requests for the same Bot can reuse. Reuse depends on matching tools and
earlier input and on where ChatGPT places cache boundaries for the model.

## Renew an allowance

When a request uses all its steps, choose **Continue for 20 more steps** on
the paused conversation message or in **Needs you**. The button shows the
request's original allowance. A request that started with eight steps gets
eight more per approval, even after a settings change or restart. Each pause
accepts one approval. When steps are available, Flock can also resume paused
work that shares the allowance and that the approver has decide access to.
If another Bot's task is paused, open that Bot's waiting item.

Each task has a separate allowance of three rounds for follow-up work in T3
or work handed off to another Bot after the initial work. Choose **Continue
for 3 more rounds** to renew it. Installation and T3 permissions still apply
to each action.

For a paused task, the CLI can renew either allowance.

```sh
flock approve TASK WAIT REVISION GENERATION
```

Use the wait ID, current revision ID (or `0` before the first revision), and
generation from `flock status TASK`.
