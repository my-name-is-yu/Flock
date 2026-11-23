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
ctest --test-dir build --output-on-failure -R '^(flock_verify_run_settlement|flock_t3|flock_core_acceptance|flock_workflow_context)$'
```

The first command explores one assigned Run, one outstanding delegated child, unrelated Thread work, a restart continuation, and an available or missing history row. The second command must exit 1. It replays #220 by letting a successful parent-only wait release a cancelled Run while its child still runs.

`t3/adapter.cpp` maps a successful delegated wait into `Observation::Kind::Finished`. The fallback applies only to the original assigned Run. `confirmsQueueCancellation` requires the exact history row with cancelled status and explicit null `startedAt`. The follow-up exact wait rules out a pending or different restart continuation. Flock assigns Run IDs from its own send or recovery of its exact prompt. T3 creates restart continuations with server-origin messages.

`Runtime::Impl::ingest` consumes `Finished` inside the SQLite transaction after checking its snapshot. The model groups the T3 observations and one matching ingest into a transition. It does not model the snapshot fence, T3 itself, retries of network writes, or multiple children. `t3/tests.cpp` exercises the adapter through loopback MCP, and `tests/core_acceptance.cpp` and `tests/workflow_context.cpp` check the persisted task and workflow behaviors.

To replay the historical branch against the adapter test in a scratch checkout, make `confirmsQueueCancellation` return the exact wait result without inspecting recent Runs. Build `flock_t3_tests` and run `flock_t3`. The check must fail with `cancelling a started Run must keep its delegated work outstanding`. Restore the source and rerun before committing.

The last source comparison was at `b43c3f8262bb15ee1282930465ec17a7673a74f8`. Move that commit forward only after checking the model against the named paths and rerunning both model variants and the runtime checks. Use `/maintain-formal-verification-skill` when updating this check.
