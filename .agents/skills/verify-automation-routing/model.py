import argparse
from collections import deque
from dataclasses import dataclass, replace
from enum import Enum, auto


class Route(Enum):
    PREVIOUS = auto()
    NEW = auto()


class Save(Enum):
    READY = auto()
    STAGED = auto()
    PUBLISHED_UNCOMMITTED = auto()
    PUBLISHED_WITHOUT_LOCK = auto()
    COMMITTED = auto()


@dataclass(frozen=True)
class Workspace:
    published: Route = Route.PREVIOUS
    saved: Route = Route.PREVIOUS
    save: Save = Save.READY
    canceled: bool = False
    fired_with: Route | None = None


def workspace_steps(state, historical):
    if state.save == Save.READY:
        if historical:
            yield "Publish the new Bot workspace before acquiring the schedule write lock", replace(
                state, published=Route.NEW, save=Save.PUBLISHED_WITHOUT_LOCK)
        else:
            yield "Stage the new schedule route under the SQLite write lock", replace(state, save=Save.STAGED)
    if state.save == Save.STAGED:
        yield "Publish the workspace while holding the schedule write lock", replace(
            state, published=Route.NEW, save=Save.PUBLISHED_UNCOMMITTED)
        yield "Schedule staging or file publication fails and the write rolls back", Workspace(
            canceled=state.canceled, fired_with=state.fired_with)
    if state.save in (Save.PUBLISHED_UNCOMMITTED, Save.PUBLISHED_WITHOUT_LOCK):
        yield "Commit the updated schedule route", replace(state, saved=Route.NEW, save=Save.COMMITTED)
    if state.save == Save.PUBLISHED_UNCOMMITTED:
        yield "SQLite commit fails and the published workspace is restored before unlocking", Workspace(
            canceled=state.canceled, fired_with=state.fired_with)
    if state.save in (Save.STAGED, Save.PUBLISHED_UNCOMMITTED):
        return
    if not state.canceled:
        yield "Cancel the configured schedule", replace(state, canceled=True)
        if state.fired_with is None:
            yield "Fire the due occurrence using the route recorded in SQLite", replace(state, fired_with=state.saved)


def workspace_failure(before, after):
    if before.fired_with is None and after.fired_with is not None and after.fired_with != before.published:
        return "A configured occurrence used the old workspace after the new workspace was published"
    return None


class Bot(Enum):
    AFFECTED = auto()
    HEALTHY = auto()


@dataclass(frozen=True)
class Delivery:
    profile_readable: bool = True
    staged: tuple[Bot, ...] = ()
    delivered: tuple[Bot, ...] = ()
    finished: bool = False


def delivery_steps(state, historical, scheduler):
    if state.finished:
        return
    if state.profile_readable:
        yield "The affected Bot's profile lookup starts failing with a filesystem error", replace(
            state, profile_readable=False)
    else:
        yield "The affected Bot's profile becomes readable again", replace(state, profile_readable=True)
    order = (Bot.AFFECTED, Bot.HEALTHY) if scheduler else (Bot.HEALTHY, Bot.AFFECTED)
    bot = order[len(state.staged)]
    unreadable = bot == Bot.AFFECTED and not state.profile_readable
    if unreadable and historical:
        failure = "Abort the due scan before its later occurrence" if scheduler else "Roll back the webhook's staged receipts"
        yield failure, replace(state, staged=(), finished=True)
        return
    staged = (*state.staged, bot)
    finished = len(staged) == len(order)
    delivered = staged if scheduler or finished else ()
    model = "its saved model" if unreadable else "its current model"
    label = "Commit the occurrence for" if scheduler else "Submit the webhook task for"
    yield f"{label} the {bot.name.lower()} Bot with {model}", replace(
        state, staged=staged, delivered=delivered, finished=finished)


def delivery_failure(_before, after):
    if after.finished and len(after.delivered) != 2:
        return "An unreadable Bot profile blocked another Bot's automation delivery"
    return None


class Phase(Enum):
    QUEUED = auto()
    EDIT_QUEUED = auto()
    DECIDING = auto()
    NOTES_WAIT = auto()
    EDIT_WAIT = auto()
    MODEL_FAILURE_WAIT = auto()
    DISPATCH_UNCONFIRMED = auto()
    RUNNING = auto()
    MODEL_FAILURE_WITH_RUN = auto()
    SETTLED = auto()


@dataclass(frozen=True)
class Project:
    affected: Phase = Phase.QUEUED
    healthy: Phase = Phase.QUEUED
    notes_readable: bool = True


def project_owner(state):
    candidates = [(phase in (Phase.QUEUED, Phase.EDIT_QUEUED), order, bot)
                  for order, (bot, phase) in enumerate(((Bot.AFFECTED, state.affected), (Bot.HEALTHY, state.healthy)))
                  if phase not in (Phase.NOTES_WAIT, Phase.EDIT_WAIT, Phase.SETTLED)]
    return min(candidates)[2] if candidates else None


def project_steps(state, historical):
    if state.notes_readable:
        yield "The older Bot's notes become unreadable", replace(state, notes_readable=False)
    else:
        yield "The older Bot's notes become readable again", replace(state, notes_readable=True)
    if state.affected == Phase.NOTES_WAIT:
        if historical == "notes-retry":
            yield "Retry the older task directly into planning", replace(state, affected=Phase.DECIDING)
        else:
            yield "Retry the older task through the project queue", replace(state, affected=Phase.QUEUED)
        if state.notes_readable:
            if historical == "notes-edit":
                yield "Save the edit while leaving the older task outside the queue", replace(state, affected=Phase.EDIT_WAIT)
            else:
                yield "Save the edit and requeue the older task", replace(state, affected=Phase.EDIT_QUEUED)

    owner = project_owner(state)
    if owner is None:
        return
    field = owner.name.lower()
    phase = getattr(state, field)
    if phase == Phase.DECIDING and owner == Bot.AFFECTED and not state.notes_readable:
        if historical == "notes":
            yield "Repeated notes failures put the older task in an ordinary model failure wait", replace(
                state, affected=Phase.MODEL_FAILURE_WAIT)
        else:
            yield "Repeated notes failures put the older task in the pre-dispatch notes wait", replace(
                state, affected=Phase.NOTES_WAIT)
        return
    if phase == Phase.RUNNING and owner == Bot.AFFECTED and not state.notes_readable:
        yield "Notes fail while the older task is answering an active Run's question", replace(
            state, affected=Phase.MODEL_FAILURE_WITH_RUN)
    if phase == Phase.MODEL_FAILURE_WITH_RUN and state.notes_readable:
        yield "Retry the active Run's question without releasing its slot", replace(state, affected=Phase.RUNNING)
    transition = {
        Phase.EDIT_QUEUED: ("Apply the saved edit before planning", Phase.QUEUED),
        Phase.QUEUED: ("Start planning", Phase.DECIDING),
        Phase.DECIDING: ("Send work without confirmed acceptance", Phase.DISPATCH_UNCONFIRMED),
        Phase.DISPATCH_UNCONFIRMED: ("Confirm a Run", Phase.RUNNING),
        Phase.RUNNING: ("Settle the Run", Phase.SETTLED),
    }.get(phase)
    if transition:
        action, next_phase = transition
        yield f"{action} for the {field} Bot", replace(state, **{field: next_phase})


def project_failure(before, after):
    owner = project_owner(after)
    if after.affected in (Phase.EDIT_WAIT, Phase.EDIT_QUEUED) and after.healthy == Phase.SETTLED and owner != Bot.AFFECTED:
        return "An edited automation stayed outside the queue after the project became idle"
    if after.affected in (Phase.NOTES_WAIT, Phase.MODEL_FAILURE_WAIT):
        expected = None if after.healthy == Phase.SETTLED else Bot.HEALTHY
        if owner != expected:
            return "An unreadable Bot's notes wait held the shared project slot"
    if before.affected == Phase.NOTES_WAIT and after.affected in (Phase.QUEUED, Phase.DECIDING):
        if before.healthy in (Phase.DECIDING, Phase.DISPATCH_UNCONFIRMED, Phase.RUNNING) and owner != Bot.HEALTHY:
            return "A retried notes wait overtook work already active in the shared project"
    if before.affected in (Phase.DISPATCH_UNCONFIRMED, Phase.RUNNING, Phase.MODEL_FAILURE_WITH_RUN):
        if after.affected != Phase.SETTLED and owner != Bot.AFFECTED:
            return "A notes failure parked a task with an active or unconfirmed dispatch"
    return None


def explore(name, initial, steps, failure):
    queue = deque([(initial, ())])
    seen = {initial}
    while queue:
        state, trace = queue.popleft()
        for action, next_state in steps(state):
            next_trace = (*trace, action)
            if reason := failure(state, next_state):
                print(f"FAIL: {reason}")
                print("\n".join(f"  {step}" for step in next_trace))
                return 1
            if next_state not in seen:
                seen.add(next_state)
                queue.append((next_state, next_trace))
    print(f"PASS: checked {len(seen)} reachable {name} states")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--historical", choices=("workspace", "scheduler", "webhook", "notes", "notes-retry", "notes-edit"))
    historical = parser.parse_args().historical
    if historical in (None, "workspace"):
        if result := explore("workspace save", Workspace(), lambda state: workspace_steps(state, bool(historical)),
                             workspace_failure):
            return result
    for kind in ("scheduler", "webhook"):
        if historical not in (None, kind):
            continue
        if result := explore(kind, Delivery(), lambda state: delivery_steps(state, bool(historical), kind == "scheduler"),
                             delivery_failure):
            return result
    if historical in (None, "notes", "notes-retry", "notes-edit"):
        return explore("shared project notes recovery", Project(), lambda state: project_steps(state, historical),
                       project_failure)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
