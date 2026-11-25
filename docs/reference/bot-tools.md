# Bot tools

Workspace, web, MCP, skill, attachment, conversation, and Git read calls
take operation arguments without a separate rationale. For decision and
Git write rationale, see [conversation behavior](runtime.md#conversation-behavior).

## Workspace and web

Conversation turns can read, write, and search files under
`<home>/workspaces/<botId>`. `workspace.read` covers reads and searches, and
`workspace.write` covers writes. Both target the Bot ID. Bots delegate
commands, scripts, builds, and tests to T3 Sessions. If a Session needs a
file it cannot access, the Bot asks you to make it available in the project.

Web fetch requires the `web/<hostname>/fetch` policy target and accepts public
HTTPS text responses. Web search requires the `web/public/search` target
and a Brave Search credential. After reading web evidence, the Bot can keep
reading and reply. Actions require a new user message.

## Shared tools

See [Shared tools](../guide/flock-settings.md#shared-tools) for Web setup.
To manage tools from the host, set `FLOCK_WEB_SEARCH_KEY` for Brave Search
and define skills and MCP servers in `<home>/bot-tools.json`. Relative paths
resolve against the Flock home.

```json
{
  "skills": {"writing": "/path/to/skills/writing"},
  "mcpServers": {
    "knowledge": {"url": "https://example.com/mcp", "tokenEnv": "KNOWLEDGE_MCP_TOKEN"}
  }
}
```

`skill.list` lists the catalog and `skill.read` reads individual skills.
The Bot calls MCP servers via Streamable HTTP with `mcp.discover`,
`mcp.invoke`, and `mcp.read_resource` targeting the configured server name.
`tokenEnv` is optional and names a host-managed bearer credential.
Discovery lists one collection per call and returns `nextCursor` when
another page is available.

## Tool batches

ChatGPT can propose several independent reads in one conversation response
when at least two batchable tools are offered. Conversation batches can read
Session search and events, environment readiness, tasks and task results,
workflow status, schedules, secret names, connections, saved tool results,
and memory and revision history. Store reads run under the Store mutex.
T3 reads can overlap.

Flock schedules conflicting Bot tools in order and saves results while
individual calls wait for approval. The Bot receives results in provider
order and can then reply or propose a separate action. Git reads must be
requested on their own. See [model call accounting](model-allowance.md)
for how tools use the originating request's allowance.
