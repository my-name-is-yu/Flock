# T3 and external MCP connections

## T3 provider registration

Connecting T3 registers a loopback MCP server with installed Codex and Claude
Code and exports their Flock skills. The provider runs on the Flock host to
reach the listener. Flock can read Sessions and queue replies through a
connected remote T3 environment's OAuth endpoint. Flock checks Session access
before accepting an ask and prefixes replies with the Bot's name.

Adding or reconnecting an environment preserves a complete provider
configuration at the same port. Changing the port or repairing a missing
registration rotates the provider bearer. Existing provider sessions may
need to reload their configuration.

Disconnect stops new Flock sends to that environment. Flock can still follow
and stop Runs it started while its T3 authorization remains valid. Disconnect
the final environment to remove the local provider registration. OAuth
revocation is separate and happens in T3 Code. See
[Connect T3](../guide/connections.md#connect-t3).

For custom provider installations, set `FLOCK_CLAUDE_CLI`,
`FLOCK_CODEX_CLI`, `FLOCK_CODEX_CONFIG`, `FLOCK_CLAUDE_CONFIG`,
`FLOCK_CLAUDE_SKILLS_DIR`, or `FLOCK_CODEX_SKILLS_DIR`.

## Web port and headless operation

The first T3 connection from Web settings uses the running Flock port. A
CLI connection uses an explicit `--port PORT`, then the saved System port,
then the existing connection port, then 4173. The explicit port must match
the System setting when one is set. A `flock run --port PORT` override must
match the port of an existing T3 registration. Change the saved port from
[System settings](../guide/flock-settings.md#system) and restart.

Run `flock run --no-web` for headless operation. Keep the Web listener enabled
on the registered port to accept T3 skill calls. An already accepted ask can
finish after a headless restart using the same Flock home, connection, and Web
identities file.

## External MCP clients

A [provisioning command](../guide/connections.md#ask-a-bot-from-an-external-mcp-client)
creates a separate credential each time and prints its `connectionId` and
`bearerToken` once. The client uses Streamable HTTP at
`http://127.0.0.1:PORT/mcp/flock/external` and sends `Authorization: Bearer TOKEN`.
For another host, use an HTTPS ingress that forwards only
`/mcp/flock/external` with the loopback Host header and the client's
Authorization header.

| MCP tool | Arguments and result |
| --- | --- |
| `flock_submit_task` | Send `bot`, `text` (up to 8,192 UTF-8 bytes), and a client-generated `requestId`. Save the returned `taskId`. Retry with the same arguments if the response is lost. |
| `flock_get_task` | Send `taskId` to poll `status`, any pending `question`, the final `result`, and `sessionLink`. T3 applies its own access controls to the link. |
| `flock_answer_question` | Send `taskId` and `text` (up to 8,000 UTF-8 bytes) with the question's `waitId`, `targetRevision`, and `targetGeneration`. Poll again if the question changed or closed. |

For a completed task, `sessionLink` points to the Session that supplied the
result. Flock returns `null` if there is no hosted T3 link, including for
local-only Sessions, or if current permissions prevent sharing it.

Flock uses the Bot's configured T3 environment or its sole eligible
connected environment. A connection can read and answer only tasks it
submitted, including after a Flock restart. Revoking a credential leaves
existing tasks and receipts in Flock.
