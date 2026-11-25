# Flock Web

The executable serves the embedded UI on its IPv4 loopback listener. The Web UI and CLI share the same SQLite Store. The HTTP layer projects explicit display fields from Bot descriptors and Store rows. Local POSTs require the server's exact origin, JSON and the Web UI request header. Canonical conversation-message and Bot-reply text remains unchanged; the Web timeline derives safe, server-rendered Markdown HTML for already-sent bubbles. The composer has no live Markdown preview. Session links use T3's environment-scoped hosted route.

The composer sends `botId`, `clientMessageId` and `text` to `POST /api/messages`. Selecting a question or prior result also sends `replyToMessageId`. The receipt identifies the saved message. For the same person and Bot, retrying the same payload with the same `clientMessageId` returns the same receipt, and reusing the key with different content returns HTTP 409. The Bot handles each message in a turn and replies in the conversation, listing any work it started or changed. Select a task question's choice and press **Send answer** to answer directly through `POST /api/operations`. Written replies to questions go through the Bot.

Bots can read, write, and search files in their own workspaces. Requests to run commands, scripts, builds, or tests go to T3 Sessions. If a Session needs a file it cannot access, the Bot asks you to make it available in the selected T3 project.

The Bot's name and **needs you** count above its chat open the items waiting on you. The Bot list also shows a count shortcut. A question card uses the composer to answer its exact target through `POST /api/inbox/decide`. A pull request review card uses the composer to send a message to the Bot through `POST /api/messages`, with the review as the reply target. The review stays in **Needs you** until you respond. **History**, in the conversation controls and the Bot's menu, lists what you have already answered or approved.

The first `GET /api/view?bot=...` returns a page of Session tasks. Pass its `sessionCursor` as `sessionsAfter` on the next view request to receive the groups that recorded a new Run. Each returned group includes its full list of Sessions. To continue paging backwards, pass the last page’s `sessionsBefore` to `GET /api/sessions?bot=...&before=...`.

## Dependencies

- [cpp-httplib v0.67.0](https://github.com/yhirose/cpp-httplib/tree/v0.67.0) is pinned by release and SHA-256 in CMake.
- [cmark 0.31.1](https://github.com/commonmark/cmark/tree/0.31.1) is pinned by release and SHA-256 in CMake for shared CommonMark rendering.
- [Open Bot Avatar 0.3.0](https://www.npmjs.com/package/open-bot-avatar/v/0.3.0) is vendored as the runtime JavaScript files from the npm tarball under `assets/vendor/open-bot-avatar`, with its MIT license. Tarball integrity is `sha512-3P0o8eiClHHiLr7lIFZE/WxUtqHLjVb0xQ0PqYzA48XG/oQlWyeIdcY0egK6H8AgFuoY1aKVhc8QKSaj4z5ZOw==`. The UI uses the public `mountAvatar`, `getExample`, and `validateAvatar` APIs with static list avatars and system reduced-motion support in the conversation header.
- [T3 Code v0.1.11 thread links](https://github.com/pingdotgg/t3code/blob/v0.1.11/packages/shared/src/threadLinks.ts) define the environment and Thread segments of the browser handoff.

The loopback listener runs as the local owner by default. For a shared listener, set
`FLOCK_WEB_IDENTITIES_FILE` before starting Flock to the path of an owner-only
JSON file (mode 0600). Provide an array with one entry per credential. Each entry
has a unique `authorization` string (the complete Basic or Bearer authorization
header), a `person` ID, and an optional boolean `adminRoot`. Keep one root
identity for installation administration. Restart the listener after changing this file.
Browsers can sign in through the Basic challenge.

## Channel reply formatting

The channel worker keeps reply Markdown unchanged in the conversation journal and
delivery outbox. Immediately before `reply` delivery it sends a derived payload
copy: Slack receives mrkdwn and iMessage receives plain text with link
destinations. Other operations and payload fields, including suggestions,
attachments, and approval state, are unchanged.

The local bridge receives the derived `payload.text` and `origin` on `/deliver`.
For Slack replies, `payload.deliveryMetadata.mrkdwn` matches the renderer's
result. The worker also sets `parse` to `none`, `link_names` to false, and
`disableAutoMentions` to true.
The provider sender must honor these settings, must not enable automatic
mentions or link-name expansion, and must send the supplied text rather than
reparse canonical Markdown. Text-like Markdown content is escaped before it
reaches Slack. `mrkdwn` is false when inline code contains a backtick, a code
block contains triple backticks, or literal text or raw HTML contains `*`, `_`,
`~`, or a backtick. In that case the entire reply uses escaped plain text with
links expanded. This removes Markdown formatting throughout the reply to
preserve literal content without changing code delimiters or inserting characters.
The provider sender is external to this repository; an adapter that ignores
this contract can re-enable provider-specific notifications.

Queued and resumed shared Web messages use the sender's current identity and
Bot access. If a person has multiple credentials, their queued messages have
root authority only when all of those credentials currently have `adminRoot`.

## Ask a Bot from a T3 Session

Start T3 Code and choose **Connect local T3** in **Flock settings → Connections**.
For another machine, use **Use a connection URL** and paste the MCP URL from T3
Code's **Settings → Environments**. From a terminal on the machine running Flock,
run `flock t3 connect`, with an MCP URL for a remote environment.
Approve T3 OAuth read and operate access in the browser on the Flock machine.
Flock registers a loopback MCP server with installed Codex and Claude Code and
exports their Flock skills. For shared Web identities, include `local-owner` in
the identity file and grant it `decide` access to the Bots it may ask.

Ask for a Bot by name or select its Flock skill in the Session. The generic
`flock` skill lists the Bots available to you. Flock checks access to the Session
and queues each Bot reply with the Bot's name. If the Bot asks you a question,
the coding agent shows it to you and forwards your reply. Ask the coding agent
to stop the Bot when you want it to leave the Session.

The first connection from the Web uses the running Flock port. The CLI uses
the port saved in **Flock settings → System**, or port 4173 if there is no
saved port or existing connection. An explicit CLI port must match the System
setting when one is set. To change the System port, save it and restart Flock.
Keep the Web listener enabled to accept
MCP calls. An accepted ask can finish when the Runtime restarts with
`flock run --no-web` and uses the same home, connection, and identity file.

Use **Reconnect** to authorize a saved environment again. Adding or reconnecting
an environment keeps a complete provider configuration at the same port.
Changing the port or repairing a missing registration rotates the provider
bearer. Existing provider sessions may need to reload their configuration.
Choose **Disconnect** in Flock settings or run `flock t3 disconnect ENVIRONMENT`
to stop new Flock sends to that environment. Flock can still follow and stop Runs
it already started while its T3 authorization is valid. To revoke that authorization,
remove Flock under **Sharing & access → Authorized clients** in T3 Code.
Disconnecting the final environment removes the local provider registration. For custom provider
installations, set `FLOCK_CLAUDE_CLI`, `FLOCK_CODEX_CLI`, `FLOCK_CODEX_CONFIG`,
`FLOCK_CLAUDE_CONFIG`, `FLOCK_CLAUDE_SKILLS_DIR`, or `FLOCK_CODEX_SKILLS_DIR`.

The provider must run on the same machine as Flock to call the loopback MCP
endpoint. Flock can read and queue replies through a connected remote T3
environment's OAuth endpoint.

## Flock settings

Use **Appearance** to set the theme for the current browser. Use **Permissions**
to set the installation's permission mode and rules. **Full** is
the default and applies only Never rules. In Supervised, Never takes
precedence over Ask first, then Allow, then the
[category defaults](../README.md#installation-policy). Rules added here or
from an approval card stay in the list until an administrator removes them.

Installation administrators manage ChatGPT, T3, and other integrations in
**Connections**. Local discovery and URL connections both use T3 OAuth.
Set up the corresponding external service or local bridge before connecting it.

Use **Access** to enable shared sign-in and manage accounts for this Flock.
Keep the initial administrator password before confirming shared access. Bot
sharing and channel sender links are managed from each Bot's **Sharing and connections** page.
If the host provides a Web identities file, Access shows those users as host-managed.

**System** shows the running and next-start values for the Web port and when
Flock reads T3 Sessions. Changes
marked for restart take effect the next time the host starts Flock. The same
applies to GitHub credentials, channel configuration, and shared tools.

An action that asks appears in the Decision Inbox and alongside its
conversation message. The requester or an installation administrator can
choose **Allow once** to resume the action or **Deny** to block another attempt
from that request. An installation administrator can choose **Allow for this
Flock** to resume the action and save an exact Allow rule for the installation.
Background cards expire after 30 minutes. An administrator can choose
**Allow next time for this Flock** to save a rule for future requests.
Cards prompted by a person's message stay open, including messages from a
linked Slack or iMessage sender. The approval is answered in Flock. Changing
modes closes pending approvals for the prior mode.

`GET /api/settings` returns the mode, rules, T3 connection details, and action
names that apply to people only. Installation roots can also check the model
connection (`model`), T3 setup progress (`t3Setup`), and whether a revoked T3
registration still needs cleanup (`t3CleanupPending`).

These endpoints require an installation root.

- `POST /api/settings/mode` takes `mode`.
- `POST /api/settings/rule` takes `kind`, `action`, `target`, and the Boolean `add`.
  Set `add` to `true` to create a rule for a Bot action with a nonblank target.
  Set `add` to `false` to remove the matching rule.
- `POST /api/settings/model-login` takes `reconsent` and starts ChatGPT sign-in in
  the browser on the machine running Flock.
- `POST /api/settings/t3-connect` takes `source` of `local`, `url` with
  `mcpUrl`, or `saved` with `environmentId`. The person who starts the
  connection receives the approval link in `t3Setup.authorizationUrl`.
- `POST /api/settings/t3-disconnect` takes `environmentId`. Pass `null` to retry
  removing the provider registration after disconnecting the environment.

## Bot settings

Each Bot's **Access** page (Flock settings → Bots → Access) lists who can use
the Bot, secrets it is waiting for, and connected computers.

The creator owns the Bot. A root identity can manage all Bots. Shared members
can view and talk to their Bot and answer its Decision Inbox items. With Web
identities configured, the owner manages members in Bot settings.

`GET /api/bot-settings?bot=ID` returns settings visible to that person.
`GET /api/settings/models?bot=ID` returns `{models: [{slug, displayName}]}`
for the Bot owner and installation roots. An unavailable catalog returns
`503` with `{"error":"models_unavailable"}`.
`POST /api/bots/profile` accepts `botId` and any of `environmentId`,
`projectId`, or `model` in one request. Omitted fields are preserved and an
empty string clears the supplied field. Name, avatar, and role are saved separately.
`POST /api/bot-settings/members` takes `botId`, `person` and boolean `add`.
`POST /api/bot-settings/secret` accepts `botId`, `requestId` and `value`.
Mutations use the usual same-origin JSON headers. Fulfilled secret values
stay inside adapters and follow the `secret.use` policy.

`POST /api/messages` optionally accepts an `attachments` array with up to four
objects containing `name`, `mimeType` and `dataBase64`. Supported types are
`image/png`, `image/jpeg`, `audio/wav` and `audio/mpeg`. Each file is limited
to 64 KiB and the attachment array to 98,000 JSON bytes. Media inspection follows the `attachment.inspect` action with the Bot ID as its target.
An image-capable ChatGPT model receives authorized image contents. Audio
inspection returns file properties and WAV format and duration.

In the Web composer, attach PNG or JPEG screenshots with the attachment button
or paste them into the message field. Each image can be up to 8,192 pixels per
side and 4,194,304 pixels total. A message still requires text. The sent message
shows the number of images attached. Bot inspection follows the `attachment.inspect` rule. Screenshot drafts and their retry keys live
in the current page and are cleared on reload.

## Memory and automation

Model input starts with the Bot's role, private memory, and earlier conversation
history. Flock puts the current event and tool results near the end to lengthen
the prefix shared by requests for the same Bot.

Use `GET /api/memory?bot=ID` for private memory. Add `docId` for a shared
document, for example `team%2Fstaging.md`. `GET /api/memory/history` accepts the
same parameters. Use `before` to page through revisions and `proposalsBefore`
to page through proposals. Use `revisionId` for the complete before and after
text or `proposalId` for a single proposal.

Post to `/api/memory/propose`, `/api/memory/apply`, or `/api/memory/revert` with
`botId`, optional `docId`, and the current `baseVersion`. A proposal takes
`content` and `rationale`. Apply takes either `content` or `proposalId`.
A revert takes `revisionId` and creates a new revision. Post `proposalId` with
the same Bot and document identifiers to `/api/memory/reject` to dismiss a
pending proposal. Shared documents require the corresponding person scope
unless the caller is the local owner or a root identity.

Use `GET /api/schedules?bot=ID` with an optional `before` cursor. Add `active=1`
to list only active entries. The first active page also includes `canManage`
and `unscheduledRequests` from the Bot's configuration. Create with
`POST /api/schedules` and `botId`, `kind`, and `request`. A `schedule` takes
`cron` and `timezone`. A `reminder` takes a future `fireAt` in Unix seconds, `timezone`,
and optional `label`. A `trigger` takes `source` and an optional object
`eventFilter` whose top-level fields must match the event exactly. Cancel with
`DELETE /api/schedules?bot=ID&id=AUTOMATION_ID` and the usual origin and
`X-Flock-Action: 1` headers.

For a webhook, post JSON with `externalEventId` and an `event` object to
`/api/webhooks/SOURCE` with `X-Flock-Action: 1`. Machine clients can omit
`Origin`. With configured identities, the sender needs an authorization header
and decide access to each Bot that should receive the event. Flock deduplicates
each Bot's delivery by source and event ID. Retries return the authorized task
receipts. Flock checks the saved source and request for protected values at
delivery. The queued task records the trigger ID, firing time, and redacted
source and event payload. The external event ID is used for deduplication.
