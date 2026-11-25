# Channel bridge protocol

The provider bridge runs separately on the Flock host. For registration and sender
linking, see
[Connect Slack or iMessage](../guide/integrations.md#connect-slack-or-imessage).

## Channel access

Flock connects to each workspace's bridge at `127.0.0.1` on its configured port. The
workspace uses its own random bearer secret of 32–4,096 printable ASCII characters
without whitespace. A Session bridge uses a distinct secret with the same constraints.

A provider sender ID maps to one Bot and one Flock person in its workspace. That person
needs access to talk to the Bot. In a shared installation the person must also resolve
to a current Flock identity. See
[shared Web identities](http-api.md#shared-web-identities).

Channel rules use `channel.use`, `channel.read`, and `channel.post` with compact JSON
targets such as `["slack","T123"]` or `["imessage","messages-on-mac"]`. Flock checks
the current sender link, Bot access, and applicable channel permissions when admitting
events or using channel resources. It rechecks permissions before sending or reconciling
queued deliveries. See [installation policy](installation-policy.md).

## Incoming events

The provider bridge authenticates native events and retrieves attachment bytes. Send
normalized JSON to `POST /api/channels/slack/events` or
`POST /api/channels/imessage/events` on Flock's loopback listener. Use
`Content-Type: application/json` and `Authorization: Bearer <workspace-secret>`.

| Field | Meaning |
| --- | --- |
| `workspaceId` | Configured workspace ID or connection name |
| `threadRef` | Stable provider thread reference. In Slack it identifies both the channel and the thread |
| `externalMessageId` | Stable provider ID for this event |
| `senderId` | Linked provider sender ID |
| `text` | Message text |
| `attachments` | Optional array of objects with `name`, `mimeType`, and `contentBase64` |
| `replyToExternalMessageId` | Optional provider message ID of the Bot post being answered |

Reuse the same external event ID, thread reference, sender, and message content when
retrying. Flock uses the channel, workspace, thread, and external message ID to identify
a delivery. A reply target must identify a single Bot post in the same channel
conversation. When a person selects a suggestion on a posted reply, including an answer
to a task question, send the suggestion's label as `text` and the post's provider ID as
`replyToExternalMessageId`. Reuse the target and choice event ID if the provider
retries.

Slack and iMessage each accept up to four files totaling 4 MiB per event, subject to the
connector's allowed media types and per-file limit. The built-in connectors accept PNG,
JPEG, WebP, and plain text. Flock checks the bytes against the MIME type, stores them as
supplied, and applies channel attachment permissions when reading them. For files read
from a T3 Session, see [Session evidence](session-evidence.md).

## Outgoing deliveries

Flock sends an authenticated JSON request to the bridge's `POST /deliver` with
`idempotencyKey`, `origin`, `operation`, and `payload`. The `origin` object contains
`channelKind`, `workspaceId`, `threadRef`, and `externalMessageId`. Apply reply
`suggestions` using the channel's available controls or plain text.

Persist each delivery identity. When the send outcome is uncertain, Flock calls
`POST /reconcile` with the same `idempotencyKey` and `origin`. Both endpoints return
HTTP 200 with a JSON object containing `state`.

| State | Meaning |
| --- | --- |
| `accepted` | The provider applied the operation. Include the provider's `externalMessageId` for an accepted post |
| `unknown` | The outcome is still uncertain. Flock continues reconciling |
| `not_sent` | Definitive evidence shows the send had no provider effect. Flock can retry with the same key |
| `rejected` | The bridge rejected the operation. Flock stops retrying it |

### Reply formatting

Flock stores the original reply Markdown in the conversation journal and outbox. Before
sending a `reply`, it derives `payload.text`. Slack receives mrkdwn, and iMessage
receives plain text with link destinations. The transformation preserves suggestions,
attachments, approval state, and other payload fields. Other operations use their
original payload.

For Slack replies, `payload.deliveryMetadata.mrkdwn` matches the renderer's result. The
same object sets `parse` to `none`, `link_names` to `false`, and `disableAutoMentions`
to `true`. The external provider sender must send the supplied text as rendered and
honor this metadata to suppress automatic mentions and link-name expansion.

Flock escapes text-like Markdown content before sending it to Slack. It sets `mrkdwn` to
`false` when inline code contains a backtick, a code block contains triple backticks, or
literal text or raw HTML contains `*`, `_`, `~`, or a backtick. In those cases the
whole reply uses escaped plain text with links expanded. This preserves literal content
without changing code delimiters or inserting characters.

## HTML previews

The preview URL is an HTTPS origin. Forward only `/api/artifacts/flock-<token>` to
Flock's loopback listener, with the loopback `Host` header. The token is a random
32-character lowercase hexadecimal string. Flock serves sandboxed HTML up to 256 KiB and
checks current workspace permission when the link is opened.

## Local machine operations

A local machine entry names the Flock host and its Flock owner. The machine owner needs
decide access to the Bot. The owner's approval lists exact absolute paths for
`read_file` or `list_directory` within a Session. Each operation checks that the
caller's Run is active for the approved Session and task, that the owner remains
authorized, and that the operation and path match an approved rule. The host opens paths
without following symlinks.

A trusted Session bridge authenticates the caller and sends the verified `environmentId`, `threadId`, and `runId`, with `machineId`, `operation`, and `path`, in JSON to
loopback `POST /api/channels/local-operations`. The bridge holds and sends the separate
Session bridge bearer secret. C++ adapters use
[`Store::runLocalMachineOperation`](../../include/flock/core.hpp) with the same
authenticated Run.
