# Memory and recurring requests

## Private memory

In your Bot's Web conversation, ask it to remember a lasting preference,
correction, convention, or decision. The Bot can also pick up a lasting point
from a direct message you send as its owner or an installation administrator.
Look for **Remembered** below the message to see the change. Choose **Undo**
while the edit is still current to restore the previous memory.

Use the conversation to ask for an explicit edit. Administrators can control Bot
edits under **Flock settings → Permissions** with the **Edit Bot memory** action.
A direct message sent with `flock message` can also trigger a memory review.

To seed a Bot's private memory, put task-independent starting notes in
`MEMORY.md` next to its `config.json` before the first memory access.
To share notes with T3 work, set `"shareMemoryWithSessions": true` in that
Bot's configuration. Check who can read the Session before sharing its notes.

See the [memory reference](../reference/memory-and-automation.md#memory) for
versioning, shared playbooks, and the HTTP API for explicit edits.

## Scheduled requests

1. Open the Bot's settings and choose **Recurring requests**.
2. Enter the request, choose a daily, weekday, or weekly repeat, and choose a
   time. Flock uses the browser's displayed time zone. Select **Add**.
3. Use **Remove** beside a request to stop future occurrences.

You can also ask the Bot in its conversation to manage a schedule or reminder,
or to follow up on a webhook. See
[automation behavior](../reference/memory-and-automation.md#automation)
for what happens after downtime or a pause.

### Configure a schedule on the host

For a schedule managed in the Bot's `config.json`, use the exact environment
and project IDs from T3 and add a `cron` entry. This example asks for an update
every Monday at 10:00 in Tokyo.

```json
{
  "environmentId": "<T3 environment ID>",
  "projectId": "<T3 project ID>",
  "cron": [
    {
      "id": "release-notes",
      "schedule": "0 10 * * 1",
      "timezone": "Asia/Tokyo",
      "request": "Summarize the open items for the next release."
    }
  ]
}
```

Configured requests appear in **Recurring requests**. Edit or remove their
entries in the file, then restart Flock to load the changes. See the
[configured schedule contract](../reference/memory-and-automation.md#configured-schedules)
for cron syntax and how Flock handles entry IDs across restarts.
