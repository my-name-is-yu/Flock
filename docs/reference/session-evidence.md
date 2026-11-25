# T3 Session search and evidence

## Access and continuation

In a personal Bot conversation, the T3 connection owner or installation root
can search readable Sessions across connected projects. A shared Bot
conversation uses its configured project scope. External channels search
Sessions recorded for the same conversation.

Flock can send a new instruction to a readable Session and track the new Run,
including Sessions originally started in T3. If the Session is busy, T3
queues the work. The [T3 send policy](installation-policy.md) and T3's Operate
permission apply.

## Search pagination

Search scans up to 64 candidates per page with a shared two-second deadline
for candidate reads. A cursor after a matched Session first rechecks that
Session's readability with its own two-second deadline. Follow `nextCursor`
with the same filters even if a page has no matches. If a timeout occurs
before the scan advances, retry with the same cursor.

Reading an attachment from a Session returns its metadata and up to 8 KiB
of text when available.

## Task evidence

By default the Bot evaluates the Session's final report. It can retry an
unavailable final report once Flock has verified which Run produced it.

Set **Flock settings → System → T3 Session information** to **When needed**
to let the Bot read Session history on demand, or **Before answering** to
prefill evidence with the same bounded reader. The CLI equivalents are
`flock run --session-read on_demand` and `flock run --session-read prefill`.
The default is **Off** (`--session-read off`). Flock allows up to three
evidence reads per event and saves history excerpts of up to 8 KiB each in
SQLite.

Session links follow the environment-scoped hosted route defined by
[T3 Code v0.1.11](https://github.com/pingdotgg/t3code/blob/v0.1.11/packages/shared/src/threadLinks.ts).
Session task pagination in the Web UI is described in the
[HTTP API reference](http-api.md#session-task-pages).
