---
name: verify-handoff-model-commit
description: Check Session handoff edits that reserve model attempts, save model usage and decisions, or retry a write after SQLite contention.
---

# Verify handoff model commits

A transient SQLite lock must leave a completed model response available for the next attempt to write it. Otherwise a paid response disappears and the last available call can exhaust the handoff budget.

Use Python 3.10 or later with its standard library. Run from the repository root.

```sh
python3 .agents/skills/verify-handoff-model-commit/model.py
python3 .agents/skills/verify-handoff-model-commit/model.py --historical
ctest --test-dir build --output-on-failure -R '^(flock_verify_handoff_model_commit|flock_participation|flock_app_headless_handoff|flock_core_acceptance)$'
```

The model explores one successful last-budget call, a contending writer, and revoked or superseded work. The historical command must exit 1. It reproduces the immediate usage write that could lose a response, a failure previously fixed for the Conversation and Task workers in #154 and #202.

`Store::Impl::callModel` in `src/model_budget.cpp` returns the checked response and usage. `SessionHandoff::checkpointModelDecision` retains them in memory. `commitModelDecision` writes usage and the authorized, still-current decision in one SQLite transaction. Terminal or revoked work can settle usage without applying a decision. The model groups that transaction into a step. It does not model provider errors, process death before the transaction, multiple runtime processes, or changes on T3.

`modelOutcomeSurvivesWriterContention` in `tests/participation.cpp` holds a real SQLite writer lock after the last allowance is spent. It checks consultation, coordination, authorization revocation, newer human input, and unavailable history during a completion retry. `flock_core_acceptance` checks that completion can be reconsidered after losing its pinned Git evidence. In a scratch checkout, discard the cached response when `commitModelDecision` throws a contended `StorageError`. Rebuild `flock_participation` and verify that it reports `SQLite contention discarded the completed handoff outcome and exhausted its last call`. Restore the source and rerun.

The last source comparison was at `18610874afd25b6361d339ad5c17a8540d2fe0df`. Review changes against the model and rerun both variants and the runtime tests before moving that commit forward. Use `/maintain-formal-verification-skill` when updating this check.
