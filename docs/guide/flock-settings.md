# Manage Flock settings

Open **Flock settings** from the Web UI. Installation administrators manage
installation-wide settings. Browser preferences apply to the current browser.

## Appearance and notifications

Use **Appearance** to choose the theme. Turn on **Notifications** and allow
them when the browser asks. Flock alerts you when a Bot replies, needs input,
or encounters a failure while a Flock tab is open. Alerts pause for the Bot
you are viewing. Select an alert to open its Bot. If the Bot editor is open,
Flock opens the conversation after you save or close the editor.

## Permissions and approvals

Use **Permissions** to choose **Full** or **Supervised** and add or remove
**Never**, **Ask first**, and **Allow** rules. See the
[installation policy](../reference/installation-policy.md) for precedence,
defaults, and matching targets. If a Bot pauses a launch for approval, check
the mode and rules here.

Open the action's card in the **Decision Inbox** or next to its originating
conversation message. Choose **Allow once** or **Deny**. Installation
administrators can use **Allow for this Flock** to approve and save a rule,
or **Allow next time for this Flock** on an expired background card. Review
[channel approval behavior](../reference/installation-policy.md#approval-cards)
if the request started outside Flock.

As a terminal alternative, inspect and change the mode and rules from the
Flock host. Quote wildcard targets.

```sh
build/app/flock mode
build/app/flock mode supervised
build/app/flock mode full
build/app/flock rules list
build/app/flock rules add ask repository.pr 'github.com/acme/*'
build/app/flock rules remove ask repository.pr 'github.com/acme/*'
```

The rule syntax is `flock rules add|remove allow|ask|never ACTION TARGET`.
For example, an `environment.launch` rule can use the quoted target `'demo-*'`.

## Connections

Use **Connections** to manage [ChatGPT and T3](connections.md),
[GitHub, Slack, iMessage, and local computers](integrations.md), and
[shared Bot tools](#shared-tools). Set up any external service or local bridge
before connecting it.

### Shared tools

Under **Connections → Shared tools**, use **Web search** to save a
**Brave Search API key**. Use **MCP servers** to register a server URL and an
optional bearer token. Use **Skills** to register an existing skill folder
under the Flock home's `skills/` directory. Restart Flock to use the saved
settings. See the [Bot tool reference](../reference/bot-tools.md#shared-tools)
for host-managed configuration and policy.

## Access

Open **Access** to enable shared sign-in and manage accounts. Save the
initial administrator password before confirming shared access. To grant
access to a Bot or link a channel sender, open **Flock settings → Bots**,
then choose **Access** for that Bot.

If the host provides a [Web identities file](../reference/http-api.md#shared-web-identities),
the users it defines appear as host-managed.

## System

Use **System** to see running and next-start values for the **Web port**
and **T3 Session information**. Change a value, save it, and restart Flock
when prompted. This also applies to changes marked for restart in
connections, including GitHub credentials, channel configuration, and
shared tools.

See the [Session evidence modes](../reference/session-evidence.md#task-evidence)
and [Web port rules](../reference/mcp.md#web-port-and-headless-operation)
for startup behavior and CLI alternatives.
