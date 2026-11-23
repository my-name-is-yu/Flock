---
name: verify-change-feed
description: Check change feed edits that can lose an older message invalidation when a watched Bot projection fails and the shared journal cursor advances.
---

# Verify change feed retries

A successful Bot view refresh must publish an unread historical message change even when the current snapshot is identical. Otherwise an open conversation can show stale older messages until the person reloads.

Use Python 3.10 or later with its standard library. Run from the repository root.

```sh
python3 .agents/skills/verify-change-feed/model.py
python3 .agents/skills/verify-change-feed/model.py --historical
ctest --test-dir build --output-on-failure -R '^(flock_verify_change_feed|flock_web)$'
```

The first command enumerates reachable states for one subscribed Bot and up to two journal writes. The second command must exit 1. It replays #153 by dropping the history obligation after a failed projection advances the cursor. The next successful projection sees an unchanged snapshot and misses the invalidation.

A projection exception leaves a refresh pending. A successful JSON null projection clears the obligation because the Bot view is unavailable to the subscriber. The model groups one monitor pass into a step. SQLite behavior, HTTP delivery, and access checks are exercised by `web/tests/http.cpp`. The model does not simulate individual subscribers or prove the C++ implementation.

The model follows `ChangeFeed::Watched` in `web/change_feed.hpp`, and `ChangeFeed::refresh` and `ChangeFeed::monitor` in `web/change_feed.cpp`. The real SQLite and SSE regression is `retryHistoryInvalidation`. To reproduce the original failure, use a scratch checkout at `a25d40a` and reverse only the production paths in `d500b4a` before rebuilding `flock_web_tests`. The test must fail with `stream missed an invalidation`.

The last source comparison was at `bc0a0405524d007e1edca4c64b6fc750bca50977`. Review changes to the named functions against the model. Move that commit forward only after checking the mapping and running both model variants and the HTTP tests. Use `/maintain-formal-verification-skill` when updating this check.
