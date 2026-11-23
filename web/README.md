# Flock Web

The executable serves the embedded UI on its IPv4 loopback listener. The Web UI and CLI share the same SQLite Store. The HTTP layer projects explicit display fields from Bot descriptors and Store rows. Local POSTs require the server's exact origin, JSON and the Web UI request header. Browser text is rendered as text, while Session links use T3's environment-scoped hosted route.

The composer sends `botId`, `clientMessageId` and `text` to `POST /api/messages`. Selecting a question or prior result also sends `replyToMessageId`. The receipt identifies the saved message. For the same person and Bot, retrying the same payload with the same `clientMessageId` returns the same receipt, and reusing the key with different content returns HTTP 409. The Bot handles each message in a turn and replies in the conversation, listing any work it started or changed. Select a task question's choice and press **Send answer** to answer directly through `POST /api/operations`. Written replies to questions go through the Bot.

The Bot's name and **needs you** count above its chat open the items waiting on you. The Bot list also shows a count shortcut. A question card uses the composer to answer its exact target through `POST /api/inbox/decide`. A pull request review card uses the composer to send a message to the Bot through `POST /api/messages`, with the review as the reply target. The review stays in **Needs you** until you respond. **History**, in the conversation controls and the Bot's menu, lists what you have already answered or approved.

The first `GET /api/view?bot=...` returns a page of Session tasks. Pass its `sessionCursor` as `sessionsAfter` on the next view request to receive the groups that recorded a new Run. Each returned group includes its full list of Sessions. To continue paging backwards, pass the last page’s `sessionsBefore` to `GET /api/sessions?bot=...&before=...`.

## Dependencies

- [cpp-httplib v0.67.0](https://github.com/yhirose/cpp-httplib/tree/v0.67.0) is pinned by release and SHA-256 in CMake.
- [Open Bot Avatar 0.3.0](https://www.npmjs.com/package/open-bot-avatar/v/0.3.0) is vendored as the runtime JavaScript files from the npm tarball under `assets/vendor/open-bot-avatar`, with its MIT license. Tarball integrity is `sha512-3P0o8eiClHHiLr7lIFZE/WxUtqHLjVb0xQ0PqYzA48XG/oQlWyeIdcY0egK6H8AgFuoY1aKVhc8QKSaj4z5ZOw==`. The UI uses the public `mountAvatar`, `getExample`, and `validateAvatar` APIs with static list avatars and system reduced-motion support in the conversation header.
- [T3 Code v0.1.11 thread links](https://github.com/pingdotgg/t3code/blob/v0.1.11/packages/shared/src/threadLinks.ts) define the environment and Thread segments of the browser handoff.

The loopback listener runs as the local owner by default. For a shared listener, set
`FLOCK_WEB_IDENTITIES_FILE` before starting Flock to the path of an owner-only
JSON file (mode 0600). Provide an array with one entry per credential. Each entry
has a unique `authorization` string (the complete Basic or Bearer authorization
header), a `person` ID, and an optional boolean `adminRoot`. Keep one root
identity for installation administration. Restart the listener after changing this file.
Browsers can sign in through the Basic challenge.

Queued and resumed shared Web messages use the sender's current identity and
Bot access. If a person has multiple credentials, their queued messages have
root authority only when all of those credentials currently have `adminRoot`.

## Ask a Bot from a T3 Session

Run `flock t3 connect ENVIRONMENT MCP_URL` on the machine running Flock.
Approve T3 OAuth read and operate access for each connected environment.
Flock registers a loopback MCP server with installed Codex and Claude Code and
exports their Flock skills. For shared Web identities, include `local-owner` in
the identity file and grant it `decide` access to the Bots it may ask.

Ask for a Bot by name or select its Flock skill in the Session. The generic
`flock` skill lists the Bots available to you. Flock checks access to the Session
and queues each Bot reply with the Bot's name. If the Bot asks you a question,
the coding agent shows it to you and forwards your reply. Ask the coding agent
to stop the Bot when you want it to leave the Session.

The first connection saves port 4173 by default. To change it, pass
`--port PORT` and restart `flock run`. Keep the Web listener enabled to accept
MCP calls. An accepted ask can finish when the Runtime restarts with
`flock run --no-web` and uses the same home, connection, and identity file.

Adding an environment at the same port keeps a complete provider configuration.
Reconnecting an existing environment, changing the port, or provisioning a
missing registration rotates the shared bearer. Existing provider sessions may
need to reload their configuration. Run `flock t3 disconnect ENVIRONMENT` to
revoke the connection to that environment. Disconnecting the final environment
revokes the bearer and removes the provider registration. For custom provider
installations, set `FLOCK_CLAUDE_CLI`, `FLOCK_CODEX_CLI`, `FLOCK_CODEX_CONFIG`,
`FLOCK_CLAUDE_CONFIG`, `FLOCK_CLAUDE_SKILLS_DIR`, or `FLOCK_CODEX_SKILLS_DIR`.

The provider must run on the same machine as Flock to call the loopback MCP
endpoint. Flock can read and queue replies through a connected remote T3
environment's OAuth endpoint.

## Flock settings

**Flock settings** shows the installation mode, rules, and the read-only T3
connection details for the person who connected T3 and installation roots.
Installation roots can choose **Full** or **Supervised** and remove rules. **Full** is
the default and applies only Never rules. In Supervised, Never takes
precedence over Ask first, then Allow, then the
[category defaults](../README.md#installation-policy).

An action that asks appears in the Decision Inbox, and inline when it has
a conversation message. **Allow once** resumes that action. **Always allow**
also saves an exact Allow rule. **Deny** blocks another attempt from that
request. The requester or installation root answers the card. Background
cards expire after 30 minutes and then offer **Always allow next time**.
Cards prompted by a person's message stay open, including messages from a
linked Slack or iMessage sender. The approval is answered in Flock. Changing
modes closes pending approvals for the prior mode.

`GET /api/settings` returns the mode, rules, and T3 connection details.
`POST /api/settings/mode` takes `mode` and `POST /api/settings/rule` removes a rule by
`kind`, `action`, and `target`. Both require an installation root.

## Bot settings

**Sharing and connections** contains shared members, pending protected inputs,
and local machine connections for the Bot.

The creator owns the Bot. A root identity can manage all Bots. Shared members
can view and talk to their Bot and answer its Decision Inbox items. With Web
identities configured, the owner manages members in Bot settings.

`GET /api/bot-settings?bot=ID` returns settings visible to that person.
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

Use `GET /api/schedules?bot=ID` with an optional `before` cursor. Create with
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
