"""Summarize flock_model_context JSONL runs as Markdown."""

import argparse
import json
from pathlib import Path
from statistics import median


def read_runs(paths):
    for path in paths:
        with path.open(encoding="utf-8") as source:
            lines = [(number, line) for number, line in enumerate(source, 1) if line.strip()]
        if not lines:
            raise ValueError(f"{path}: no runs")
        for number, line in lines:
            try:
                yield path.stem, json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{number}: {error.msg}") from error


def total(calls, key):
    values = [call.get(key) for call in calls]
    if values and all(value is not None for value in values):
        return sum(values)
    return None


def display(value):
    if value is None:
        return "n/a"
    return f"{value:,}" if isinstance(value, int) else f"{value:,.1f}"


def metrics(calls):
    input_tokens = total(calls, "providerInputTokens")
    cached = total(calls, "providerCachedInputTokens")
    unread = input_tokens - cached if input_tokens is not None and cached is not None else None
    rate = f"{cached / input_tokens:.2%}" if input_tokens and cached is not None else "n/a"
    prefixes = [
        call["commonInputPrefixBytes"]
        for call in calls
        if call.get("callIndex", 0) > 1 and call.get("commonInputPrefixBytes") is not None
    ]
    rereads = sum(
        action in {"expand_context", "read_tool_result"}
        for call in calls
        for action in call["actions"]
    )
    return [
        display(len(calls)), display(input_tokens), display(cached), rate, display(unread),
        display(total(calls, "localRequestBytes")), display(total(calls, "transportRequestBytes")),
        display(median(prefixes) if prefixes else None), str(rereads),
    ]


def report_table(headers, rows):
    print("| " + " | ".join(headers) + " |")
    print("| " + " | ".join("---" for _ in headers) + " |")
    for row in rows:
        print("| " + " | ".join(row) + " |")


MEASURE_HEADERS = [
    "Calls", "Input", "Cached", "Cache read", "Uncached", "Local B", "HTTP body B",
    "Median input prefix B", "Evidence rereads",
]


def identify(label, run):
    return [
        label.replace("|", r"\|"), run["case"], f'{run["mode"]} · {run["model"]}',
    ]


def runs_table(runs):
    def rows():
        for name, run in runs:
            turns = run["turns"]
            success = f'{sum(turn["success"] for turn in turns)}/{len(turns)}'
            yield identify(name, run) + metrics(run["calls"]) + [display(run["wallMs"]), success]

    report_table(["Run", "Case", "Mode and model", *MEASURE_HEADERS, "Wall ms", "Turns passed"], rows())


def call_groups_table(runs, per_turn):
    def rows():
        for name, run in runs:
            calls = run["calls"]
            if per_turn:
                groups = [
                    (str(turn["turnIndex"]), [
                        call for call in calls if call["turnIndex"] == turn["turnIndex"]
                    ])
                    for turn in run["turns"]
                ]
            else:
                groups = [("All", calls)]
            for turn, group in groups:
                for first, title in ((True, "First"), (False, "Later")):
                    selected = [call for call in group if call["firstInTurn"] == first]
                    yield identify(name, run)[:2] + [turn, title] + metrics(selected) + [
                        display(total(selected, "localCallMs")),
                    ]

    report_table(
        ["Run", "Case", "Turn", "Call", *MEASURE_HEADERS, "Model call ms"],
        rows(),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--per-turn", action="store_true", help="split first and later calls for every turn")
    parser.add_argument("paths", type=Path, nargs="+", metavar="RUN.jsonl")
    args = parser.parse_args()
    try:
        runs = list(read_runs(args.paths))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    runs_table(runs)
    print()
    call_groups_table(runs, args.per_turn)


if __name__ == "__main__":
    main()
