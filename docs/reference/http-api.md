# Loopback HTTP API

Flock serves the embedded UI on an IPv4 loopback listener. The HTTP layer
projects explicit display fields from Bot descriptors and Store rows. Browser
POSTs require the server's exact `Origin`, `Content-Type: application/json`,
and `X-Flock-Action: 1`. Protocol endpoints such as
[GitHub webhooks](github.md), [channel bridges](channel-bridge.md),
[automation webhooks](memory-and-automation.md#webhook-delivery), and [MCP](mcp.md)
use their own request contracts.

## Shared Web identities

The loopback listener runs as the local owner by default. An administrator can
[enable shared sign-in](../guide/flock-settings.md#access) in Flock settings.
For host-managed credentials, set `FLOCK_WEB_IDENTITIES_FILE` before startup
to the path of an owner-only JSON file (mode 0600). Its array contains one
entry per credential. Each entry has a unique `authorization` string with the
complete Basic or Bearer authorization header, a `person` ID, and an optional
Boolean `adminRoot`. Keep a root identity for installation administration.
Browsers can sign in through the Basic challenge. Restart the listener after
changing the file.

Queued and resumed shared Web messages use the sender's current identity and
Bot access. If a person has multiple credentials, their queued messages have
root authority only when all current credentials for that person have
`adminRoot`.

## Messages and decisions

Send `POST /api/messages` with `botId`, `clientMessageId`, and `text`. Include
`replyToMessageId` to reply to a question or prior result. The receipt identifies
the saved message. For the same person and Bot, retrying the same payload
with the same `clientMessageId` returns the original receipt. Reusing that key
with different content returns HTTP 409.

Written replies to task questions go through the Bot. The Web UI submits
selected task question choices directly through `POST /api/operations`.
An inbox question card targets its exact item through `POST /api/inbox/decide`.
A PR review card sends through `POST /api/messages` with the review as the
reply target. That review stays in **Needs you** until the user responds.

Flock stores canonical message and reply text unchanged. The timeline derives
safe, server-rendered Markdown HTML for sent bubbles. The composer has no
live Markdown preview.

## Attachments

`POST /api/messages` accepts an optional `attachments` array with up to four
objects containing `name`, `mimeType`, and `dataBase64`. PNG and JPEG files
can each be up to 2 MiB. WAV and MP3 files can each be up to 64 KiB. The
request limit fits four 2 MiB images plus Base64 and JSON overhead. Flock
checks the file signature against the MIME type. Image inspection checks
decoding, a limit of 8,192 pixels per side, and 4,194,304 pixels total.
Inspection follows the Bot's `attachment.inspect` rule. An image-capable
ChatGPT model can receive images the Bot is authorized to inspect. Audio
inspection returns file properties and, for WAV, format and duration.

The Web composer accepts up to four selected or pasted PNG or JPEG screenshots,
each up to 20 MiB before upload. It preserves images already within 2,048
pixels on the longest side and 1.75 MiB. For larger images, it resizes them
to fit and chooses the smaller PNG or JPEG. Sending requires text and the
message shows its screenshot count. Draft screenshots live on the current
page and reload clears them.

See the [channel bridge](channel-bridge.md) and
[Session evidence](session-evidence.md) references for their attachment rules.

## Session task pages

The first `GET /api/view?bot=ID` returns a page of Session task groups.
Pass its `sessionCursor` as `sessionsAfter` on the next view request to get
groups that recorded a new Run, each with its full list of Sessions. To page
backwards, pass the last page's `sessionsBefore` as `before` to
`GET /api/sessions?bot=ID&before=CURSOR`.

## Installation settings

`GET /api/settings` returns the mode, rules, T3 connection details, and
person-only action names. Installation roots can also inspect the model
connection (`model`), T3 setup progress (`t3Setup`), and whether a revoked
registration needs cleanup (`t3CleanupPending`).

These mutations require an installation root.

| Endpoint | JSON body |
| --- | --- |
| `POST /api/settings/mode` | `mode` |
| `POST /api/settings/rule` | `kind`, `action`, `target`, and Boolean `add`. Use `true` to create a rule for a Bot action with a nonblank target. Use `false` to remove the matching rule. |
| `POST /api/settings/model-login` | `reconsent`. Starts ChatGPT sign-in in the browser on the Flock host. |
| `POST /api/settings/t3-connect` | `source` of `local`, `url` with `mcpUrl`, or `saved` with `environmentId`. The person starting the connection receives the approval link in `t3Setup.authorizationUrl`. |
| `POST /api/settings/t3-disconnect` | `environmentId`. Pass `null` to retry provider registration removal after disconnect. |

## Bot settings

The creator owns a Bot. The local owner manages Bots created through the CLI
or configuration. An installation root can manage all Bots. Shared members
can view and talk to the Bot and answer its Decision Inbox items. With shared
Web identities, the Bot owner manages members in Bot settings.

| Endpoint | Contract |
| --- | --- |
| `GET /api/bot-settings?bot=ID` | Returns settings visible to the caller. |
| `POST /api/bots/pause` | Takes `botId` and Boolean `paused`. A fresh message to a paused Bot returns HTTP 409 with `error: "bot_paused"` and a plain message. |
| `GET /api/settings/models?bot=ID` | For the Bot owner and installation roots. Returns `models` and `fallbackReasoningEfforts`. Models include `slug`, `displayName`, and `reasoningEfforts` when ChatGPT reports levels. An unavailable catalog returns HTTP 503 with `error: "models_unavailable"` and `fallbackReasoningEfforts`. |
| `POST /api/bots/profile` | Takes `botId` and any of `environmentId`, `projectId`, `model`, `reasoningEffort`, or a positive integer `modelCallLimit` in one request. Omitted fields are preserved. An empty string clears a supplied environment, project, model, or effort. Changing the model resets effort to **Model default** unless the request supplies an effort for the new model. Save Boolean `fixPullRequests` on its own with `botId`. Name, avatar, and role are saved separately from run settings. |
| `POST /api/bot-settings/members` | Takes `botId`, `person`, and Boolean `add`. |
| `POST /api/bot-settings/secret` | Takes `botId`, `requestId`, and `value`. Fulfilled values stay inside adapters and follow the [`secret.use` policy](installation-policy.md#c-adapter-contract). |

See [memory and automation](memory-and-automation.md) for the memory, schedule,
and automation webhook endpoints.
