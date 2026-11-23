from collections import deque
from dataclasses import dataclass, replace
from enum import Enum, auto
import sys


class Refresh(Enum):
    NONE = auto()
    PROJECTION = auto()
    HISTORY = auto()


@dataclass(frozen=True)
class State:
    journal: int = 0
    cursor: int = 0
    pending: Refresh = Refresh.NONE
    published: int = 0
    denied_through: int = 0


def successors(state, historical):
    if state.journal < 2:
        yield "Write a historical message update in SQLite", replace(state, journal=state.journal + 1)
    if state.journal > state.cursor or state.pending != Refresh.NONE:
        dirty = state.pending == Refresh.HISTORY or state.journal > state.cursor
        failed = Refresh.HISTORY if dirty and not historical else Refresh.PROJECTION
        yield "The Bot view fails and the shared cursor advances", replace(
            state, cursor=state.journal, pending=failed)
        yield "The unchanged Bot view loads successfully", replace(
            state, cursor=state.journal, pending=Refresh.NONE,
            published=state.journal if dirty else state.published)
        yield "The Bot view loads with access denied", replace(
            state, cursor=state.journal, pending=Refresh.NONE,
            denied_through=state.journal)


def main(historical):
    queue = deque([(State(), ())])
    seen = {State()}
    while queue:
        state, trace = queue.popleft()
        for action, next_state in successors(state, historical):
            next_trace = (*trace, action)
            if (action == "The unchanged Bot view loads successfully"
                    and max(next_state.published, next_state.denied_through) < next_state.cursor):
                print("FAIL: The Bot view recovered, but the history invalidation was lost")
                print("\n".join(f"  {step}" for step in next_trace))
                return 1
            if next_state not in seen:
                seen.add(next_state)
                queue.append((next_state, next_trace))
    print(f"PASS: checked {len(seen)} reachable change feed states")
    return 0


if __name__ == "__main__":
    if sys.argv[1:] not in ([], ["--historical"]):
        raise SystemExit("usage: model.py [--historical]")
    sys.exit(main(sys.argv[1:] == ["--historical"]))
