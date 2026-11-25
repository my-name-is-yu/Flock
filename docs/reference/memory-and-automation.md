# Memory and automation reference

For Web and host setup, see [Memory and recurring requests](../guide/memory-and-automation.md).

## Memory

Flock imports the Bot's `MEMORY.md` on the first private memory access.
After the import, SQLite supplies versioned private memory for conversation and
task decisions. The current role is loaded separately. A private memory
review sees only the current direct message and the Bot's existing private
memory. Automatic saves update matching entries and preserve unrelated entries.
Bot edits follow the `memory.write` [installation rules](installation-policy.md)
with the Bot ID as the target.

With `shareMemoryWithSessions` enabled, Flock redacts protected values and
appends a memory excerpt after the work instruction. The excerpt, heading,
and any truncation marker fit in the smaller of 8,192 bytes and the remaining
Session prompt budget. A follow-up can include changed notes or a longer
excerpt when more space is available. Anyone with read access to the Session
can see the notes in its history.

Shared playbooks use an explicit document ID and the `shared_memory.read`
and `shared_memory.write` actions. HTTP access checks both the caller's
permissions and the Bot's policy for the document. The local owner and
installation roots have the person-side shared memory scope.

### Memory HTTP API

For authentication and mutation headers, see the [HTTP API](http-api.md).

Read private memory with `GET /api/memory?bot=ID`. Add `docId` for a shared
document, for example `team%2Fstaging.md`. The result includes `docKey`,
`content`, `version`, and `updatedAt`.

`GET /api/memory/history` takes the same Bot and document parameters. Use
`before` to page through revisions and `proposalsBefore` to page through
proposals. Each collection returns up to 20 entries, with its next cursor
or `0` when exhausted. Use `revisionId` for the full `previousContent` and
`newContent`, or `proposalId` for a single proposal with its `proposedContent`.

Each mutation takes `botId` and, for a shared document, `docId`. Propose, apply,
and revert require the document's current `baseVersion`.

| Endpoint | Additional fields and effect |
| --- | --- |
| `POST /api/memory/propose` | `content` and `rationale` create a pending proposal. |
| `POST /api/memory/apply` | Either `content` or `proposalId` creates a new revision. The proposal must be pending and based on the current version. |
| `POST /api/memory/revert` | `revisionId` restores the original revision's prior content as a new revision. |
| `POST /api/memory/reject` | `proposalId` dismisses a pending proposal. |

Content can be up to 8,192 bytes. A proposal rationale can be up to 1,000 bytes.
Reads require Bot view access. Mutations require Bot settings access.

## Automation

A missed one-time reminder fires once when the runtime restarts. Missed
recurrences produce one task with the first and last scheduled times and
occurrence count. The schedule then resumes in its configured IANA time zone.
Scheduling supports dates through 2199. A pause skips recurrences and reminders
due through the time the Bot resumes, including downtime. Webhook events skipped
during a pause stay skipped when the same event is retried.

### Configured schedules

Flock syncs configured `cron` entries to SQLite at startup and when it saves
the Bot's run settings. Keeping an entry's `id` and timing unchanged preserves
its next occurrence. Omit an entry or use an empty `cron` array to deactivate
configured schedules. Restoring an entry resumes it at its next scheduled
occurrence. A schedule canceled through Flock stays canceled across syncs.

Cron takes five numeric fields in minute, hour, day of month, month, and
day of week order. It supports wildcards, lists, ranges, and steps. Both
`0` and `7` mean Sunday.

### Schedule HTTP API

Use the usual [HTTP authentication and mutation headers](http-api.md).

`GET /api/schedules?bot=ID` returns `automations` and a `before` cursor.
Each page returns up to 20 entries. Pass the cursor to get the previous page
or stop at `0`. Add `active=1` for active entries only. The first active page
also includes `canManage` and `unscheduledRequests` from the Bot's configuration.

`POST /api/schedules` takes `botId`, `kind`, and `request`. The request can be
up to 16,000 bytes.

| `kind` | Additional fields |
| --- | --- |
| `schedule` | `cron` and `timezone`. |
| `reminder` | `fireAt` in Unix seconds, an IANA `timezone`, and an optional `label`. The time must be in the future. |
| `trigger` | `source` and an optional `eventFilter` object. Every filter key must match the corresponding top-level event field exactly. |

Cancel with `DELETE /api/schedules?bot=ID&id=AUTOMATION_ID`. Listing requires
Bot view access. Creating and canceling require Bot settings and decide access.

### Webhook delivery

Send a JSON object with `externalEventId` and an `event` object to
`POST /api/webhooks/SOURCE` with `X-Flock-Action: 1`. Machine clients can omit
`Origin` and `Sec-Fetch-Site`. With configured identities, the sender needs an
authorization header and decide access to each Bot that should receive the
event.

Flock deduplicates delivery for each Bot by source and external event ID.
Retries return the task receipts the sender is currently authorized to see.
Flock checks the saved trigger's destination and filters against protected
values at delivery and redacts the request. The queued task records the
trigger ID, firing time, and redacted source and event payload.
