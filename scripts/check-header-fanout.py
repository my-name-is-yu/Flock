#!/usr/bin/env python3
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import sys

LIMIT = 30
HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".inc", ".ipp", ".tpp")
MAKE_RULE = re.compile(r"(?:\\.|[^:\\])+:[ \t]+(.*)")
MAKE_WORD = re.compile(r"(?:\\[ \t#:\\]|[^ \t])+")
MAKE_ESCAPE = re.compile(r"\\([ \t#:\\])|\$\$")


def dependencies(depfile):
    first_rule = depfile.read_text(errors="surrogateescape").replace("\\\n", " ").split("\n", 1)[0]
    match = MAKE_RULE.fullmatch(first_rule)
    if not match:
        raise ValueError(f"Cannot read object dependencies from {depfile}")
    return (
        MAKE_ESCAPE.sub(lambda escape: escape[1] or "$", word)
        for word in MAKE_WORD.findall(match[1])
    )


def header_fanout(source, build):
    source = source.resolve()
    build = build.resolve()
    counts = Counter()
    objects = 0
    for depfile in build.rglob("*.o.d"):
        target = next((p for p in depfile.parents if p.parent.name == "CMakeFiles"), None)
        if target is None:
            continue
        objects += 1
        headers = set()
        for entry in dependencies(depfile):
            if not entry.endswith(HEADER_SUFFIXES):
                continue
            path = Path(os.path.abspath(target.parent.parent / entry))
            if path.is_relative_to(source) and not path.is_relative_to(build):
                headers.add(path.relative_to(source).as_posix())
        counts.update(headers)
    if not objects:
        raise ValueError(f"No CMake object depfiles in {build}. Build with Unix Makefiles first.")
    if not counts:
        raise ValueError(f"No project headers under {source} in the depfiles from {build}")
    return objects, counts


def main():
    parser = argparse.ArgumentParser(description="Check project header fan-out in a built Makefiles tree.")
    parser.add_argument("source", type=Path)
    parser.add_argument("build", type=Path)
    parser.add_argument("allowlist", type=Path)
    args = parser.parse_args()
    try:
        allowed = json.loads(args.allowlist.read_text())
        if not isinstance(allowed, dict) or any(type(count) is not int or count <= LIMIT for count in allowed.values()):
            raise ValueError(f"Expected an object mapping allowlisted headers to measured fan-out in {args.allowlist}")
        objects, counts = header_fanout(args.source, args.build)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")

    violations = []
    for name in sorted(counts.keys() | allowed.keys()):
        count = counts[name]
        if name in allowed:
            recorded = allowed[name]
            if count != recorded:
                violations.append(f"{name}: measured {count}, recorded {recorded}")
        elif count > LIMIT:
            violations.append(f"{name}: measured {count}, limit {LIMIT}")
    if violations:
        print("Header fan-out check failed", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1
    print(f"Header fan-out passed for {len(counts)} headers across {objects} objects")
    return 0


if __name__ == "__main__":
    sys.exit(main())
