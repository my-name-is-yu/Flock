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

Run `./scripts/verify.sh` to configure a fresh build with tests, build it, and
run CTest.

## Get your first Bot reply

1. Start Flock with `build/app/flock run` and open the printed Web URL.
   Flock keeps its state in `--home DIRECTORY`, `FLOCK_HOME`, or
   `~/.config/flock`. A database from a build with another schema version
   is refused, so use a new state directory.
2. In **Flock settings → Connections**, choose **Sign in with ChatGPT** and
   allow plan usage. Start T3 Code and choose **Connect local T3**, or use
   **Use a connection URL** for a T3 server on another machine.
3. Select **New Bot** in the sidebar, choose a preset or **Start from scratch**,
   enter a name, and choose **Create**. Open the Bot's settings and choose a
   **Model** and T3 destination under **How it runs**. Edit **Role** to tell
   the Bot how to help you.
4. Send a message such as “What can you help me with?” to see the reply in
   the Bot's conversation.

To try the conversation flow without connecting accounts, run
`build/app/flock --home "$(mktemp -d)" run --fixed`, create a Bot, and
choose **Demo** as its model.
