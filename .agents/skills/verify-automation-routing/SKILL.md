---
name: verify-automation-routing
description: Check configured schedule publication, due automation scans, and webhook delivery when Bot workspace settings change or a Bot profile becomes unreadable.
---

# Verify automation routing

Use Python 3.10 or later with its standard library. Run from the repository root.

```sh
python3 .agents/skills/verify-automation-routing/model.py
python3 .agents/skills/verify-automation-routing/model.py --historical workspace
python3 .agents/skills/verify-automation-routing/model.py --historical scheduler
python3 .agents/skills/verify-automation-routing/model.py --historical webhook
ctest --test-dir build --output-on-failure -R '^(flock_verify_automation_.*|flock_app|flock_memory_automation|flock_web_git_webhooks)$'
```

The first command must pass. Each historical command must exit 1 and print the interleaving that violates the corresponding invariant.

## Workspace publication

The model explores one configured occurrence, a workspace edit, cancellation, and retry after staging, publication, or commit fails. A firing must use the route published before that firing began. The historical variant uses `e43cd7d`, where `Bots::updateWorkspace` replaces `config.json` and then synchronizes SQLite. A firing between the two operations reads the old route. [PR #329 review](https://github.com/my-name-is-yu/Flock/pull/329)

`Bots::updateRunSettings` in `app/bots.cpp` now uses `Store::syncSchedules` in `src/automation.cpp`. The Store holds its mutex and an immediate SQLite write transaction from staging through file publication and commit. The app restores the previous file on a failed commit before releasing the lock. The model keeps the same due occurrence through these transitions and allows retries within its one-edit bound. File rollback failures, process death, external editors and future occurrences are outside the model. `runSettingsChangesResyncConfiguredSchedules` and `failedScheduleSyncLeavesWorkspaceUnchanged` exercise actual SQLite state in `app/tests/schedules.cpp`.

## Delivery isolation

The model explores two eligible Bots and one local filesystem error that can persist or recover. For scheduled work, the affected Bot is first in the due scan and each occurrence has its own transaction. For a webhook, the unaffected Bot goes first and both receipts commit together. Both Bots must still receive tasks. The historical variant reproduces how the filesystem exception escaped the model lookup and aborted the scan or rolled back the whole webhook on `2a8f04f`. [PR #327 review](https://github.com/my-name-is-yu/Flock/pull/327), [PR #330 review](https://github.com/my-name-is-yu/Flock/pull/330)

`Store::Impl::fireDueAutomations` and `Store::receiveWebhook` in `src/automation.cpp` share `Store::Impl::automationRoute`. It uses the saved `config` when `Bots::plannerModel` in `app/bots.cpp` returns no current planner. The model treats each successful routing and task write as one step. Its scope covers profile availability and transaction outcomes. SQLite internals, future scans, authorization, and webhook filtering are outside this model. `unreadableProfileKeepsAutomationDelivery` in `app/tests/schedules.cpp` exercises the lookup, fallback, and persisted delivery through SQLite.

## Replay against the app

In a scratch checkout, move the `writeAtomic` publication in `Bots::updateRunSettings` before `Store::syncSchedules`, using its overload that takes the prepared schedules. Drop the file restoration branch. `flock_app` must reject a published workspace that disagrees with a due task after a rejected SQLite route write. For a delivery replay, let the filesystem error from `Bots::tryGet` propagate. `flock_app` must fail at the due scan. Run the same fixture with webhook delivery first to observe the error there as well. Restore the code and rerun the checks.

The last source comparison was at `3f9f6b6e3c9031b4739fab085e00c6d1c68855a8`. Move it forward after checking the mapping and rerunning the current and historical models and the app tests. Use `/maintain-formal-verification-skill` to update the model.
