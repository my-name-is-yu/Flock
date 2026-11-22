# Flock Web

The executable serves the embedded UI on its IPv4 loopback listener. The Web UI and CLI share the same SQLite Store. The HTTP layer projects explicit display fields from Bot descriptors and Store rows. Local POSTs require the server's exact origin, JSON and the Web UI request header. Browser text is rendered as text, while Session links use T3's environment-scoped hosted route.

The composer sends `botId`, `clientMessageId` and `text` to `POST /api/messages`. Selecting a question or prior result also sends `replyToMessageId`. The receipt identifies the saved message. Retrying the same payload returns the same receipt, and reusing the key with different content returns HTTP 409. The Bot handles each message in a turn and replies in the conversation, listing any work it started or changed. Tapping one of a task question's choices answers that question directly through `POST /api/operations` without a conversation turn; a written reply to the question goes through the Bot. The CLI `submit` command and `POST /api/requests` each create an explicit new Task.

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

An administrator can GET `/api/access?bot=ID` and POST to `/api/access/person`,
`resource`, `decision`, `secret` and `bot-change` under the same prefix. These
routes require the usual same-origin JSON mutation headers. Grant mutations use
`botId`, `action` (grant or revoke) and either `person` and `role` or
`resourceKind`, `resourceId` and `operation`. Decisions use a saved `requestId`
and boolean `approve`. Protected input uses a saved `requestId` and `value`.

A root can assign a person's delegation authority with `POST /api/access/person-scope`
using `botId`, `person`, `action` and the exact resource tuple. An ordinary
administrator needs both admin access to the receiving Bot and personal authority
for the resource operation to approve it. Bot change decisions record human
approval for subsequent profile administration.
