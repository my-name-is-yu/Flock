# Flock v0

Flock runs Bots locally. Each Bot owns the requests it receives, replies in
its conversation, and delegates work to T3 Sessions. The CLI and embedded
Web UI share a SQLite database.

## Build

Use a C++20 compiler, CMake 3.24+, and development libraries for SQLite
3.35+, libcurl 7.85+, and OpenSSL Crypto.

```sh
cmake --preset release
cmake --build --preset release -j
```

Install with `cmake --install build/release`. Run `./scripts/verify.sh` for the
full test suite. It requires Python 3.10+, Node.js, Chrome or Chromium, and the
OpenSSL CLI.

C++ formatting uses clang-format 21: run `./scripts/format.sh` to apply it.
`git config blame.ignoreRevsFile .git-blame-ignore-revs` skips the formatting
commit in blame.

## Get your first Bot reply

1. Start Flock with `build/release/app/flock run` and open the printed Web URL.
   Flock keeps its state in `--home DIRECTORY`, `FLOCK_HOME`, or
   `~/.config/flock`. A database from a build with another schema version
   is refused, so use a new state directory.
2. In **Flock settings → Connections**, choose **Sign in with ChatGPT** and
   allow plan usage. Start T3 Code and choose **Connect local T3**, or use
   **Use a connection URL** for a T3 server on another machine.
3. Select **New Bot** in the sidebar, choose a preset or **Start from scratch**,
   enter a name, and optionally choose a T3 project under **Where does it
   work?**. Flock preselects the last project you assigned if it is available.
   Choose **Create**, then set a **Model** under **How it runs**. You can also
   change the project there and edit **Role** to tell the Bot how to help you.
4. Send a message such as “What can you help me with?” to see the reply in
   the Bot's conversation.

To try the conversation flow without connecting accounts, run
`build/release/app/flock --home "$(mktemp -d)" run --fixed`, create a Bot, and
choose **Demo** as its model.
