# Flock v0

Flock runs Bots locally. Each Bot owns the requests it receives, replies in
its conversation, and delegates work to T3 Sessions. The CLI and embedded
Web UI share a SQLite database.

## Build

Use a C++20 compiler, CMake 3.24+, and development libraries for SQLite
3.35+, libcurl 7.85+, OpenSSL Crypto, libpng 1.6+, and libjpeg. Test builds
require Python 3.10+. Some tests also use Node.js, Chromium, or the OpenSSL CLI.

```sh
cmake -S . -B build
cmake --build build -j
```

Run `build/app/flock` from the repository root. See
[development](docs/development.md) for verification and dependency details.

## Get your first Bot reply

1. Start Flock with `build/app/flock run` and open the printed Web URL.
   When upgrading, check [state directory and schema](docs/reference/runtime.md#local-state)
   before opening existing data.
2. In **Flock settings → Connections**, choose **Sign in with ChatGPT** and
   allow plan usage. Start T3 Code and choose **Connect local T3**, or use
   **Use a connection URL** for a T3 server on another machine.
3. Select **New Bot** in the sidebar, choose a preset or **Start from scratch**,
   enter a name, and choose **Create**. Open the Bot's settings and choose a
   **Model** under **How it runs**. Edit **Role** to tell the Bot how to help you.
4. Send a message such as “What can you help me with?” to see the reply in
   the Bot's conversation. To delegate work, follow
   [Configure a Bot](docs/guide/bots.md) to select its T3 destination.

To try the same conversation flow without connecting accounts, start with the
[offline demo](docs/guide/offline-demo.md).

## Guides

- [Configure a Bot and use its conversation](docs/guide/bots.md)
- [Connect ChatGPT and T3, and ask a Bot from a Session or an MCP client](docs/guide/connections.md)
- [Manage Flock settings, access, and approvals](docs/guide/flock-settings.md)
- [Manage memory and scheduled requests](docs/guide/memory-and-automation.md)
- [Connect GitHub, Slack, iMessage, and local computers](docs/guide/integrations.md)

## Reference

- [Runtime, Bot configuration, and conversation behavior](docs/reference/runtime.md)
- [Loopback HTTP API](docs/reference/http-api.md)
- [T3 and external MCP connections](docs/reference/mcp.md)
- [Installation policy and C++ adapter contracts](docs/reference/installation-policy.md)
- [Session search and evidence limits](docs/reference/session-evidence.md)
- [Model call allowance and accounting](docs/reference/model-allowance.md)
- [Bot tools and limits](docs/reference/bot-tools.md)
- [Memory, scheduled requests, and webhooks](docs/reference/memory-and-automation.md)
- [GitHub and PR notifications](docs/reference/github.md)
- [Channel and local machine bridge protocols](docs/reference/channel-bridge.md)
