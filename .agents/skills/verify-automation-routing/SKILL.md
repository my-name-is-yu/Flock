---
name: verify-automation-routing
description: Check configured schedule publication, automation delivery, and shared project queues when Bot workspace settings change or a Bot profile becomes unreadable.
---

# Verify automation routing

Use Python 3.10 or later with its standard library. Run from the repository root.

```sh
python3 .agents/skills/verify-automation-routing/model.py
python3 .agents/skills/verify-automation-routing/model.py --historical workspace
python3 .agents/skills/verify-automation-routing/model.py --historical scheduler
python3 .agents/skills/verify-automation-routing/model.py --historical webhook
python3 .agents/skills/verify-automation-routing/model.py --historical notes
python3 .agents/skills/verify-automation-routing/model.py --historical notes-retry
python3 .agents/skills/verify-automation-routing/model.py --historical notes-edit
ctest --test-dir build --output-on-failure -R '^(flock_verify_automation_.*|flock_app|flock_memory_automation|flock_web_git_webhooks)$'
```

The first command must pass. Each historical command must exit 1 and print the interleaving that violates the corresponding invariant.

## Workspace publication

The model explores one configured occurrence, a workspace edit, cancellation, and retry after staging, publication, or commit fails. A firing must use the route published before that firing began. The historical variant uses `e43cd7d`, where `Bots::updateWorkspace` replaces `config.json` and then synchronizes SQLite. A firing between the two operations reads the old route. [PR #329 review](https://github.com/my-name-is-yu/Flock/pull/329)

`Bots::updateRunSettings` in `app/bots.cpp` now uses `Store::syncSchedules` in `src/automation.cpp`. The Store holds its mutex and an immediate SQLite write transaction from staging through file publication and commit. The app restores the previous file on a failed commit before releasing the lock. The model keeps the same due occurrence through these transitions and allows retries within its one-edit bound. File rollback failures, process death, external editors and future occurrences are outside the model. `runSettingsChangesResyncConfiguredSchedules` and `failedScheduleSyncLeavesWorkspaceUnchanged` exercise actual SQLite state in `app/tests/schedules.cpp`.

## Delivery isolation

The model explores two eligible Bots and one local filesystem error that can persist or recover. For scheduled work, the affected Bot is first in the due scan and each occurrence has its own transaction. For a webhook, the unaffected Bot goes first and both receipts commit together. Both Bots must still receive tasks. The historical variant reproduces how the filesystem exception escaped the model lookup and aborted the scan or rolled back the whole webhook on `2a8f04f`. [PR #327 review](https://github.com/my-name-is-yu/Flock/pull/327), [PR #330 review](https://github.com/my-name-is-yu/Flock/pull/330)

`Store::Impl::fireDueAutomations` and `Store::receiveWebhook` in `src/automation.cpp` share `Store::Impl::automationRoute`. It uses the saved `config` when `Bots::plannerModel` in `app/bots.cpp` returns no current planner. The model treats each successful routing and task write as one step. Its scope covers profile availability and transaction outcomes. SQLite internals, future scans, authorization, and webhook filtering are outside this model. `unreadableProfileKeepsAutomationDelivery` in `app/tests/schedules.cpp` exercises the lookup, fallback, and persisted delivery through SQLite.

## Notes recovery in the project queue

The bounded project model starts with two queued automation tasks in one project. Repeated failures to load the older Bot's notes caused a `model_failure` wait that kept the slot on `36bb5e8`. The historical notes replay demonstrates that the healthy Bot stays blocked. The notes retry replay keeps the new wait outside the queue and restores the old `deciding` retry transition. That admits the older task ahead of already active work. [PR #333 review](https://github.com/my-name-is-yu/Flock/pull/333)

`Bots::soul` in `app/bots.cpp` reports `BotNotesUnavailable`. `Runtime::Impl::recordFailure` in `src/runtime.cpp` opens `bot_notes_unavailable` after repeated failures when `automation_before_dispatch` in `src/project_slots.cpp` proves the task has no dispatch. `project_route_queue` excludes that wait. `Store::Impl::operate` in `src/store.cpp` and `Store::Impl::acceptInstruction` in `src/conversation_actions.cpp` close the wait and return the task to `queued` on retry or a conflicting edit. A dispatch with uncertain acceptance and an active Run retain the slot. The model groups repeated failures and explores one affected and one healthy task, including an active Run's question. Its scope excludes peer exchanges, multi-route Runs, and actual filesystem and SQLite calls. `unavailableAutomationNotesDoNotOwnTheProject` in `app/tests.cpp` exercises both recovery paths through Runtime and real SQLite.

## Replay against the app

In a scratch checkout, move the `writeAtomic` publication in `Bots::updateRunSettings` before `Store::syncSchedules`, using its overload that takes the prepared schedules. Drop the file restoration branch. `flock_app` must reject a published workspace that disagrees with a due task after a rejected SQLite route write. For a delivery replay, let the filesystem error from `Bots::tryGet` propagate. `flock_app` must fail at the due scan. Run the same fixture with webhook delivery first to observe the error there as well. For the notes replay, run the `unavailableAutomationNotesDoNotOwnTheProject` fixture on `36bb5e8`. It must report that unreadable automation notes prevented the healthy Bot from running. On the fixed revision, change the notes retry in `Store::Impl::operate` to `deciding`. The fixture must report that the retried request overtook an active Run. Restore the code and rerun the checks.

For the edit replay, run the fixture with `NotesRecovery::Edit` on `8d523af`. The edit saves and the task stays outside the queue. The test must report that editing the paused automation did not requeue it behind the active Run.

The last source comparison was at `a83d8aeb1d98f9833b67ebd56d3912a1dffd031f`. Move it forward after checking the transition mapping and rerunning the current and historical models and the app tests. Use `/maintain-formal-verification-skill` to update the model.
