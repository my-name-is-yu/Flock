---
name: verify-run-settlement
description: Check changes to T3 Run observation, delegated settlement, queued cancellation, or the SQLite stop barrier in Runtime.
---

# Verify T3 Run settlement

Flock can release an assigned Run only when its delegated work and restart continuation have settled. A premature release can mark the task cancelled or admit another Run while delegated work still acts.

Use Python 3.10 or later with its standard library. Run from the repository root.

```sh
python3 .agents/skills/verify-run-settlement/model.py
python3 .agents/skills/verify-run-settlement/model.py --historical
ctest --test-dir build --output-on-failure -R '^(flock_verify_run_settlement(_historical)?|flock_t3|flock_t3_multi_environment|flock_core_acceptance|flock_workflow_context)$'
```

The first command explores one assigned Run, one outstanding delegated child, unrelated Thread work, a restart continuation, and an available or missing history row. The second command must exit 1. It replays #220 by letting a successful parent-only wait release a cancelled Run while its child still runs.

In `t3/adapter.cpp`, `RunSettlement::confirm` accepts a delegated wait. It creates a finished observation from successful delegated settlement or confirmed cancellation before start. `cancelledBeforeStarting` requires the exact history row with cancelled status and explicit null `startedAt`. The follow-up exact wait rules out a pending or different restart continuation. Flock assigns Run IDs from its own send or recovery of its exact prompt. T3 creates restart continuations with server-origin messages.

`Runtime::Impl::ingest` consumes `Finished` inside the SQLite transaction after checking its snapshot. The model groups the T3 observations and one matching ingest into a transition. Its scope excludes the snapshot fence, T3 itself, retries of network writes, and multiple children. `t3/tests.cpp` exercises the adapter through loopback MCP. `cancelledRunWaitsForDelegatedWork` in `t3/multi_environment_tests.cpp` checks that the actual runtime and SQLite queue keep the next request blocked until the delegated work settles. `tests/core_acceptance.cpp` and `tests/workflow_context.cpp` check other persisted task and workflow behaviors.

To replay the historical branch in a scratch checkout, remove the early return on `cancelledBeforeStarting` from `RunSettlement::confirm`. Build both `flock_t3_tests` and `flock_t3_multi_environment_tests`. `flock_t3` must fail with `cancelling a started Run must keep its delegated work outstanding`. `flock_t3_multi_environment` must fail with `a cancelled parent freed the project's slot before its delegated work settled`. Restore the source and rerun before committing.

The last source comparison was at `8e2dbc3c8a1053aa2cbfc0685497b2b8c9197c5f`. Move that commit forward only after checking the model against the named paths and rerunning both model variants and the runtime checks. Use `/maintain-formal-verification-skill` when updating this check.
