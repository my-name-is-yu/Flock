from collections import deque
from dataclasses import dataclass, replace
from enum import Enum, auto
import sys


class Reply(Enum):
    NOT_SENT = auto()
    UNREAD = auto()
    STATIC = auto()
    ANIMATED = auto()


@dataclass(frozen=True)
class Batch:
    requests: tuple[int | None, ...]
    animation_generation: int | None


@dataclass(frozen=True)
class Read:
    batch: Batch
    contains_reply: bool


@dataclass(frozen=True)
class State:
    visible: bool = True
    generation: int = 0
    live: bool = True
    reply: Reply = Reply.NOT_SENT
    queued: Batch | None = None
    in_flight: Read | None = None


def enqueue(state, generation, variant):
    if not state.queued:
        return replace(state, queued=Batch((generation,), generation))
    prior = state.queued.animation_generation
    selected = prior if prior == generation or (variant == "--ignore-catch-up" and generation is None) else None
    return replace(state, queued=Batch((*state.queued.requests, generation), selected))


def current(state, batch):
    generation = batch.animation_generation
    return generation is None or (state.visible and generation == state.generation)


def successors(state, variant):
    if state.reply in (Reply.STATIC, Reply.ANIMATED):
        return
    if state.reply == Reply.NOT_SENT:
        yield "A Bot posts one new reply", replace(state, reply=Reply.UNREAD)
    if state.generation == 0:
        yield "The tab becomes hidden and closes the stream", replace(state, visible=False, live=False, generation=1)
    if not state.queued or len(state.queued.requests) < 3:
        if not state.visible:
            resumed = replace(state, visible=True, generation=2)
            yield "The tab returns and requests catch-up", enqueue(resumed, None, variant)
        elif state.live:
            yield "A connected stream requests a live refresh", enqueue(state, state.generation, variant)
            lost = replace(state, live=False)
            yield "The stream disconnects and requests catch-up", enqueue(lost, None, variant)
        else:
            ready = replace(state, live=True)
            yield "The replacement stream is ready and requests catch-up", enqueue(ready, None, variant)
    if state.queued and not state.in_flight:
        if current(state, state.queued):
            read = Read(state.queued, state.reply == Reply.UNREAD)
            yield "Start the queued conversation read", replace(state, queued=None, in_flight=read)
        else:
            yield "Discard a stale queued live refresh", replace(state, queued=None)
    if state.in_flight:
        read = state.in_flight
        reply = state.reply
        if current(state, read.batch) and read.contains_reply:
            can_animate = read.batch.animation_generation is not None or variant == "--historical"
            reply = Reply.ANIMATED if state.visible and can_animate else Reply.STATIC
        yield "The conversation read finishes", replace(state, reply=reply, in_flight=None)


def violation(state, next_state):
    if next_state.reply not in (Reply.STATIC, Reply.ANIMATED):
        return None
    requests = state.in_flight.batch.requests
    live_in_this_view = state.visible and set(requests) == {state.generation}
    if next_state.reply == Reply.ANIMATED and not live_in_this_view:
        return "A catch-up refresh animated a reply"
    if next_state.reply == Reply.STATIC and live_in_this_view:
        return "A foreground live reply did not animate"
    return None


def main(variant):
    queue = deque([(State(), ())])
    seen = {State()}
    while queue:
        state, trace = queue.popleft()
        for action, next_state in successors(state, variant):
            next_trace = (*trace, action)
            failure = violation(state, next_state)
            if failure:
                print(f"FAIL: {failure}")
                print("\n".join(f"  {step}" for step in next_trace))
                return 1
            if next_state not in seen:
                seen.add(next_state)
                queue.append((next_state, next_trace))
    print(f"PASS: checked {len(seen)} reachable reply motion states")
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    if args not in ([], ["--historical"], ["--ignore-catch-up"]):
        raise SystemExit("usage: model.py [--historical | --ignore-catch-up]")
    sys.exit(main(args[0] if args else None))
