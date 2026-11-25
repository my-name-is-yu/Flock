# Configure a Bot

## Create a Bot and choose how it runs

Choose **New Bot** in the Web sidebar. Pick a preset or **Start from scratch**,
then name the Bot and choose **Create**. Select **Open settings** from the Bot's
menu. Edit **Role** to set its instructions and decision criteria.

In **General → How it runs**, select a **Model** after
[connecting ChatGPT](connections.md#connect-chatgpt). Lower **Reasoning effort**
can reduce response time and reasoning token use. Select **Model default**, or
try another supported effort if ChatGPT rejects the current choice. Set
**Steps per request** for future work and use **Continue** from the conversation
or **Needs you** when a request needs more time with the model. See the
[allowance and renewal rules](../reference/model-allowance.md).

To pin where the Bot starts work, enter the canonical T3 **Environment** and
**Project** IDs in the same editor. Choose both for new Sessions and
[scheduled requests](memory-and-automation.md#scheduled-requests). For
existing Sessions or automatic destination selection, see
[how Flock chooses a destination](../reference/runtime.md#t3-destinations).
Make any files that delegated work needs available in the selected T3 project.

## Use the conversation

- Send a new request, ask for progress, add instructions, or ask the Bot
  to stop work. Describe the request in your own words. If the Bot needs
  help identifying it, answer the question it asks or select a suggested reply.
- To continue in an earlier T3 Session, ask the Bot to find and instruct it.
  For example, “Find the login investigation and add a test in that Session.”
  See [Session access and search](../reference/session-evidence.md).
- Add a screenshot by choosing a PNG or JPEG file or pasting it into the
  message box. Include a text message. See the
  [composer and attachment limits](../reference/http-api.md#attachments).
- Select the Bot's name and **needs you** count above the conversation,
  or the count in the Bot list, to open waiting items. Answer a question
  or review request with the composer. Use **Send answer** to submit a
  selected task question choice directly. Open **History** from the
  conversation controls or Bot menu to see items you answered or approved.
- When an action asks for permission, use its card to decide in Flock. See
  [permissions and approvals](flock-settings.md#permissions-and-approvals).

## Pause or share a Bot

In **General**, use **Pause** or **Resume**. Bot owners and installation
administrators also have these controls in the Bot's menu. Look for **Paused**
in the sidebar or conversation header. The composer offers **Resume** if you
manage the Bot. After resuming, send a new message to start work. See the
[pause behavior](../reference/runtime.md#pausing-and-resuming).

In **Flock settings → Bots**, choose **Access** for a Bot to see its members,
pending secret requests, and connected computers. To manage accounts, go to
[shared sign-in](flock-settings.md#access). See
[Bot ownership and access](../reference/http-api.md#bot-settings) for what
different users can do.

## Configure a Bot from files

As an alternative to Web settings, place `config.json` and `SOUL.md` in
`bots/<botId>/` in the [Flock home](../reference/runtime.md#local-state). Use
`SOUL.md` for the Bot's instructions and decision criteria. See the
[Bot configuration reference](../reference/runtime.md#bot-configuration)
for a complete example, provider settings, and worktree selection. Set up
[starting memory](memory-and-automation.md#private-memory) or
[configured schedules](memory-and-automation.md#scheduled-requests) separately.

When a request is waiting on invalid Session settings, correct the Bot's
settings and use **Check again** to retry the unsent action. Edit
`config.json` for Session options that are only set there, then reconcile the
task.

Use `allowedBots` to configure which other Bots this Bot may consult or send
work to. Create each recipient with its own role and settings. See the
[Bot-to-Bot routing rules](../reference/runtime.md#bot-to-bot-requests).

## Verify a commit on two machines

Connect both T3 machines. To give them stable names in a Bot's conversation,
add aliases with canonical T3 IDs to its `config.json`. Each route can use
its saved project selection if you leave its project unspecified. See
[T3 destinations](../reference/runtime.md#t3-destinations).

```json
{
  "conversationEnvironments": {
    "local Mac": {
      "environmentId": "<local environment ID>",
      "projectId": "<local project ID>"
    },
    "Mac mini": {
      "environmentId": "<mini environment ID>",
      "projectId": "<mini project ID>"
    }
  }
}
```

In one Flock message, ask the Bot to verify a full Git commit SHA on both
aliases and specify the checks. Both projects need the commit in their local
Git repositories. Read each Run's result and the actual SHA from its Git
command output in the request. Follow its Session link if a Run needs input.
Ask the Bot to stop the request to stop both Runs.

## Use the CLI

Run these commands from the repository root, using the same
[Flock home](../reference/runtime.md#local-state) as the running daemon.

```sh
build/app/flock message release "Check the upcoming release"
build/app/flock tasks
build/app/flock status TASK_ID
build/app/flock history TASK_ID
build/app/flock answer TASK_ID WAIT_ID REVISION_ID GENERATION "Use main"
```

See [CLI message receipts](../reference/runtime.md#cli-messages) to match a
message to the task it started. A CLI request can trigger a
[browser notification](flock-settings.md#appearance-and-notifications).
