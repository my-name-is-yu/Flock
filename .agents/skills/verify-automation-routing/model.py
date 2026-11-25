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
    parser.add_argument("--historical", choices=("workspace", "scheduler", "webhook"))
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
