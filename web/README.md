# Flock Web

The executable serves the embedded UI on its IPv4 loopback listener. The Web UI and CLI share the same SQLite Store. The HTTP layer projects explicit display fields from Bot descriptors and Store rows. Local POSTs require the server's exact origin, JSON and the Web UI request header. Browser text is rendered as text, while Session links use T3's environment-scoped hosted route.

The composer sends `botId`, `clientMessageId` and `text` to `POST /api/messages`. Selecting a question or prior result also sends `replyToMessageId`. The receipt identifies the saved message. Retrying the same payload returns the same receipt, and reusing the key with different content returns HTTP 409. The Bot handles each message in a turn and replies in the conversation, listing any work it started or changed. Select a task question's choice and press **Send answer** to answer directly through `POST /api/operations`. Written replies to questions go through the Bot. The CLI `submit` command and `POST /api/requests` each create an explicit new Task.

The Bot's name and **needs you** count above its chat open its pending decisions. The Bot list also shows a count shortcut. A question card uses the composer to answer its exact target through `POST /api/inbox/decide`. A pull request review card uses the composer to send a message to the Bot through `POST /api/messages`, with the review as the reply target. The review remains pending until a decision is recorded. Recent decisions are available from the conversation controls and the Bot's menu.

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
identity for initial grants. Restart the listener after changing this file.
Browsers can sign in through the Basic challenge.

Queued and resumed shared Web messages use the sender's current identity and
Bot access. If a person has multiple credentials, their queued messages have
root authority only when all of those credentials currently have `adminRoot`.
Send the message again if it was queued before Flock recorded the source of
shared Web messages and is rejected after an upgrade.

## Session handoffs

Run `flock t3 connect ENVIRONMENT MCP_URL` on the machine running Flock.
Approve T3 OAuth read and operate access for each connected environment.
Flock registers one loopback MCP server with installed Codex and Claude Code
2.1.327 or later and exports their Flock skills. If you replace a Bot directory
by hand, give the new Bot a new ID so old requests and cached skills keep their
original identity. For shared Web identities, include the CLI's `local-owner`
person in the identity file and give it `decide` on the Bots it may invoke.

Ask for a Bot in an active T3 Thread or select the provider's Flock skill
(`/skill:flock-<bot>` or `$flock-<bot>`, depending on the provider). The generic
`flock` skill discovers eligible Bots. Use a consultation for one reply or
request coordination with a goal for ongoing work. Flock looks for the exact
tool call in the active Run across connected environments and uses the proving
environment and Thread for replies.

One mode applies to all connected environments. The initial default, `full`,
permits consultation, coordination, and specialist delegation. Set
`--mode consult` to allow consultations only. Adding an environment without
`--mode` keeps the saved mode. In full-mode coordination the live connection
authorizes specialists to launch inside the handoff's environment and project.
Specialists still need a valid role and must be configured as peers of the
root Bot. Nested assignments also require the immediate requester's peer
configuration. Ordinary Bot launches still require their environment grants.

The first connection saves port 4173 by default. Use `--port PORT` to change
it and restart any running `flock run`. Keep the Web listener enabled for T3
skills. A busy saved port prevents startup. Adding an environment at the same
port keeps the shared provider configuration. Reconnecting an existing
environment or changing the port rotates the shared bearer. Existing provider
sessions may need to reload their configuration. `flock t3 disconnect
ENVIRONMENT` removes the environment from handoff verification and delivery.
Disconnecting the final environment revokes the bearer and removes the provider
registration. T3 OAuth credentials remain available for separately authorized
ordinary Bot work. For custom provider installations, set `FLOCK_CLAUDE_CLI`,
`FLOCK_CODEX_CLI`, `FLOCK_CODEX_CONFIG`, `FLOCK_CLAUDE_SKILLS_DIR`, or
`FLOCK_CODEX_SKILLS_DIR`.

Only providers running on the same machine as Flock can call its loopback
MCP endpoint. Flock can read, verify, and queue replies through a reachable
remote T3 MCP endpoint using that environment's OAuth credential. A provider
running on that remote machine cannot initiate a Flock handoff through the local
registration. Pinned specialist work also requires a loopback T3 MCP endpoint
because Flock checks the workspace on its own filesystem.

Flock queues Bot messages through the T3 API, so someone can keep typing in the
T3 composer. T3 orders submitted inputs. Flock rechecks observed human progress
before sending guidance, but a human submission can still race a send.
Pause in Flock before intervening if you need to halt new coordinator messages
and work. **Resume coordination** continues observation. **Stop coordination**
ends the local coordination. Already-dispatched messages may still reach T3,
and admitted specialist work may still finish. The handoff view shows the
queued or unresolved messages and admitted work separately from local state.

## Access administration

An administrator can GET `/api/access?bot=ID` and POST to `/api/access/person`,
`resource`, `decision`, `secret` and `bot-change` under the same prefix. These
routes require the usual same-origin JSON mutation headers. Grant mutations use
`botId`, `action` (grant or revoke) and either `person` and `role` or
`resourceKind`, `resourceId` and `operation`. Decisions use a saved `requestId`
and boolean `approve`. Protected input uses a saved `requestId` and `value`.
Supplying a named secret requires Bot admin access and personal authority for
that secret, or a root identity. The Bot needs its own `secret:use` grant to
resolve the value.

A root can assign a person's delegation authority with `POST /api/access/person-scope`
using `botId`, `person`, `action` and the exact resource tuple. An ordinary
administrator needs both admin access to the receiving Bot and personal authority
for the resource operation to approve it. Approving a Bot change applies the
saved name to the exact target ID. Creation requires a root identity. Rename
requires admin access to the requesting and target Bots. If the profile write
fails after approval, use **Apply approved change** to retry.

`POST /api/messages` optionally accepts an `attachments` array with up to four
objects containing `name`, `mimeType` and `dataBase64`. Supported types are
`image/png`, `image/jpeg`, `audio/wav` and `audio/mpeg`. Each file is limited
to 64 KiB and the attachment array to 98,000 JSON bytes. Grant
`attachment/<botId>/inspect` to let the Bot inspect media in its conversation.
An image-capable ChatGPT model receives authorized image contents. Audio
inspection returns file properties and WAV format and duration.

In the Web composer, attach PNG or JPEG screenshots with the attachment button
or paste them into the message field. Each image can be up to 8,192 pixels per
side and 4,194,304 pixels total. A message still requires text. The sent message
shows the number of images attached. Bot inspection requires the existing
`attachment/<botId>/inspect` grant. Screenshot drafts and their retry keys live
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
