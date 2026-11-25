# Development

## Build and verify

Use the [build prerequisites](../README.md#build) and run these commands from
the repository root for an incremental build.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
build/app/flock --help
```

For one-off verification, run `./scripts/verify.sh`. It configures a fresh
build with tests, builds it, and runs CTest. It removes the temporary build
directory on exit. Pass `--keep-build` to keep the directory for debugging.
Set `CMAKE_BUILD_PARALLEL_LEVEL` to control concurrency. Verification runs
locally. CTest registers the optional JavaScript, browser, and TLS tests when
their tools are available.

CMake pins [cpp-httplib 0.67.0](https://github.com/yhirose/cpp-httplib/tree/v0.67.0)
and [cmark 0.31.1](https://github.com/commonmark/cmark/tree/0.31.1) by release and
SHA-256. cmark provides shared CommonMark rendering. The dependencies and
checksums are in the [root build](../CMakeLists.txt) and [Web build](../web/CMakeLists.txt).

The Web UI vendors the JavaScript runtime of
[Open Bot Avatar 0.3.0](https://www.npmjs.com/package/open-bot-avatar/v/0.3.0)
with its MIT license under
[`web/assets/vendor/open-bot-avatar`](../web/assets/vendor/open-bot-avatar).
The npm tarball integrity is
`sha512-3P0o8eiClHHiLr7lIFZE/WxUtqHLjVb0xQ0PqYzA48XG/oQlWyeIdcY0egK6H8AgFuoY1aKVhc8QKSaj4z5ZOw==`.
Flock uses the public `mountAvatar`, `getExample`, and `validateAvatar` APIs
with static list avatars and system reduced-motion support in the conversation
header.

## Verification models

Use the project skills for the invariants covered by their small executable
models and their corresponding product regressions. The
[root CMake test registrations](../CMakeLists.txt) run each current model and
replay historical variants that must reproduce the named failure.

| Skill | Verified behavior |
| --- | --- |
| [Automation routing](../.agents/skills/verify-automation-routing/SKILL.md) | Published routing, isolation from unreadable Bot profiles, and recovery of unsent work waiting for notes on shared projects |
| [Change feed](../.agents/skills/verify-change-feed/SKILL.md) | Delivery of an older message invalidation after its Bot projection recovers |
| [Reply motion](../.agents/skills/verify-reply-motion/SKILL.md) | Motion for live replies and suppression during catch-up |
| [Run settlement](../.agents/skills/verify-run-settlement/SKILL.md) | Release of a Run after delegated work and continuations settle, and cancellation before a queued Run starts |

To compare single-call and batch timings, model attempts, budget units, and
budget pauses with deterministic fake providers, run
`ctest --test-dir build -R '^flock_tool_batch_measure$' -V`.

## Live checks

After [signing in to ChatGPT](guide/connections.md#connect-chatgpt), use the
[six-trial model acceptance command](reference/runtime.md#model-acceptance)
to compare how a live model applies different completion criteria. For a
live T3 check, send a Bot a request that delegates work. You can also
[verify the same commit on two T3 machines](guide/bots.md#verify-a-commit-on-two-machines).

The [offline demo](guide/offline-demo.md) exercises the request flow with
fixed adapters.
