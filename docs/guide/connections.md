# Connect ChatGPT and T3

Run `build/app/flock run` on the Flock host and open the printed Web URL.
Installation administrators manage connections in **Flock settings →
Connections**. Keep the runtime running to handle scheduled requests and
monitor Sessions after closing the conversation.

## Connect ChatGPT

Under **ChatGPT**, choose **Sign in with ChatGPT**. Finish authorization in
the browser on the machine running Flock and grant use of your eligible
ChatGPT plan. If Flock shows **Plan usage unavailable**, choose **Allow plan
usage**. Then [choose a model for each Bot](bots.md#create-a-bot-and-choose-how-it-runs).

From a terminal on the Flock host, you can also use the CLI. Check that status
shows `planUsage: true` and use a slug from the catalog in the
[Bot configuration](../reference/runtime.md#bot-configuration).

```sh
build/app/flock model login
build/app/flock model status
build/app/flock model models
```

Run `build/app/flock model login --reconsent` if plan usage is missing.

## Connect T3

Start T3 Code and select **Connect local T3** in **Connections**. For another
machine, copy its MCP URL from T3 Code's **Settings → Environments** and paste
it into **Use a connection URL**. Approve T3 OAuth read and Operate access in
the browser on the Flock machine.

Choose **Reconnect** to authorize a saved environment again. Choose
**Disconnect** to stop new sends. To revoke Flock's authorization as well,
remove it under **Sharing & access → Authorized clients** in T3 Code. See
[T3 provider registration](../reference/mcp.md#t3-provider-registration) for
credential and disconnect behavior.

As an alternative, run these commands from the Flock host. Omit the URL for
local discovery.

```sh
build/app/flock t3 connect
build/app/flock t3 connect MCP_URL
build/app/flock t3 reconnect ENVIRONMENT
build/app/flock t3 disconnect ENVIRONMENT
```

To change the saved Web port, use [System settings](flock-settings.md#system)
and restart Flock. See the [port rules](../reference/mcp.md#web-port-and-headless-operation)
before passing `--port PORT`.

## Ask a Bot from a T3 Session

After connecting T3, keep Flock's Web listener running. When using shared Web
identities, include `local-owner` in the identity file and grant it `decide`
access to the Bots it may ask. See [shared Web identities](../reference/http-api.md#shared-web-identities).

In a Session, ask for a Bot by name or select its skill with
`/skill:flock-<bot>` or `$flock-<bot>`. Use the generic `flock` skill to list
available Bots. Watch for the reply in the same Session. If the Bot asks a
question, tell the coding agent your answer so it can forward it. Ask that
agent to stop the Bot when you want it to leave the Session.

## Ask a Bot from an external MCP client

Keep the Flock Web listener running. On the Flock host, use the same state
directory as the runtime and create a client credential. Use `local-owner`
with local access, or specify an existing Flock user who can make decisions
for every Bot you include.

```sh
build/app/flock external-mcp connect local-owner release
```

Save the returned `connectionId` and `bearerToken`. Configure the client for
the [Streamable HTTP endpoint](../reference/mcp.md#external-mcp-clients)
and supply the token as a Bearer credential. Submit work, save the task ID,
and poll it to read results and answer questions. Use the same request ID if a
submission response is lost, as described in the
[tool contract](../reference/mcp.md#external-mcp-clients).

To revoke the client credential, run
`build/app/flock external-mcp disconnect CONNECTION_ID`.
