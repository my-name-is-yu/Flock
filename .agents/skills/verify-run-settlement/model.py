from collections import deque
from dataclasses import dataclass, replace
import sys


@dataclass(frozen=True)
class State:
    phase: str = "queued"
    started: bool = False
    child_active: bool = False
    unrelated_active: bool = False
    restart_pending: bool = False
    same_run: bool = True
    history_visible: bool = True
    released: bool = False


def observe(state, historical):
    if state.phase != "cancelled":
        return state
    exact_wait = not state.restart_pending
    wide_wait = exact_wait and not (state.child_active or state.unrelated_active)
    queued_cancellation = (state.same_run and state.history_visible and not state.started)
    fallback = exact_wait and (historical or queued_cancellation)
    return replace(state, released=wide_wait or fallback)


def successors(state, historical):
    if state.released:
        return
    if state.phase == "queued":
        yield "Start the assigned Run", replace(state, phase="running", started=True)
    if state.phase == "running" and not state.child_active:
        yield "Start delegated work", replace(state, child_active=True)
        if state.same_run:
            yield "Follow a restart continuation", replace(state, same_run=False)
    if state.phase != "cancelled":
        yield "Cancel the assigned Run", replace(state, phase="cancelled")
    if state.child_active:
        yield "Settle the delegated work", replace(state, child_active=False)
    if not state.unrelated_active:
        yield "Start unrelated Thread work", replace(state, unrelated_active=True)
    if not state.restart_pending:
        yield "Schedule a restart continuation", replace(state, restart_pending=True)
    else:
        yield "Resolve the pending restart", replace(state, restart_pending=False, same_run=False)
    if state.history_visible:
        yield "Lose the assigned Run history row", replace(state, history_visible=False)
    yield "Observe and apply settlement", observe(state, historical)


def violation(state):
    if state.released and state.child_active:
        return "Flock released the Run while its delegated work was active"
    if state.released and state.restart_pending:
        return "Flock released the Run while its restart was pending"
    return None


def main(historical):
    queue = deque([(State(), ())])
    seen = {State()}
    while queue:
        state, trace = queue.popleft()
        for action, next_state in successors(state, historical):
            next_trace = (*trace, action)
            failure = violation(next_state)
            if action == "Observe and apply settlement" and state.phase == "cancelled":
                if (not state.restart_pending and not state.child_active and not state.unrelated_active
                        and not next_state.released):
                    failure = "Confirmed delegated settlement did not release the Run"
                if (state.same_run and state.history_visible and not state.started
                        and not state.restart_pending and state.unrelated_active and not next_state.released):
                    failure = "Unrelated work blocked an assigned Run cancelled before start"
            if failure:
                print(f"FAIL: {failure}")
                print("\n".join(f"  {step}" for step in next_trace))
                return 1
            if next_state not in seen:
                seen.add(next_state)
                queue.append((next_state, next_trace))
    print(f"PASS: checked {len(seen)} reachable Run settlement states")
    return 0


if __name__ == "__main__":
    if sys.argv[1:] not in ([], ["--historical"]):
        raise SystemExit("usage: model.py [--historical]")
    sys.exit(main(sys.argv[1:] == ["--historical"]))
