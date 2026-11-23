from collections import deque
from dataclasses import dataclass, replace
import sys


@dataclass(frozen=True)
class State:
    outcome: str = "not-called"
    writer_locked: bool = False
    current: bool = True
    usage_saved: bool = False
    decision_saved: bool = False


def successors(state, historical):
    if state.outcome == "not-called" and not state.writer_locked and state.current:
        yield "Reserve the last model call and receive its response", replace(state, outcome="pending")
    if state.writer_locked:
        yield "Release the SQLite writer lock", replace(state, writer_locked=False)
    else:
        yield "Another SQLite connection takes the writer lock", replace(state, writer_locked=True)
    if state.current:
        yield "Revoke access or stop the handoff", replace(state, current=False)
    if state.outcome == "pending" and state.writer_locked:
        yield "The SQLite write encounters the lock", replace(
            state, outcome="lost" if historical else "pending")
    if state.outcome in ("pending", "lost") and not state.writer_locked:
        yield "Poll after SQLite can write", replace(
            state, outcome="done", usage_saved=state.outcome == "pending",
            decision_saved=state.outcome == "pending" and state.current)


def main(historical):
    queue = deque([(State(), ())])
    seen = {State()}
    while queue:
        state, trace = queue.popleft()
        for action, next_state in successors(state, historical):
            next_trace = (*trace, action)
            if action == "Poll after SQLite can write" and (
                    not next_state.usage_saved or next_state.decision_saved != state.current):
                print("FAIL: SQLite recovered without accounting for or correctly applying the paid response")
                print("\n".join(f"  {step}" for step in next_trace))
                return 1
            if next_state not in seen:
                seen.add(next_state)
                queue.append((next_state, next_trace))
    print(f"PASS: checked {len(seen)} reachable handoff checkpoint states")
    return 0


if __name__ == "__main__":
    if sys.argv[1:] not in ([], ["--historical"]):
        raise SystemExit("usage: model.py [--historical]")
    sys.exit(main(sys.argv[1:] == ["--historical"]))
