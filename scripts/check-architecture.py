#!/usr/bin/env python3
import argparse
from pathlib import Path
import re
import sys

ALLOWED = {
    "include": {"include"},
    "src": {"src", "include"},
    "t3": {"t3", "include"},
    "web": {"web", "include"},
    "app": {"app", "include"},
}
SOURCE_SUFFIXES = (".cpp", ".hpp")
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.MULTILINE)
TEST_FILE = re.compile(r"(^|_)tests\.cpp$")
SINGLE_OWNERS = (
    (re.compile(r"set_content\([^;]*application/json"), "web/http.cpp", "send JSON responses with web::reply or web::error"),
    (re.compile(r'"0123456789abcdef"(\[|;)|\bEVP_Digest\(|\bSHA256\('), "src/digest.cpp", "encode hex with encodeHex and hash with sha256Hex"),
    (
        re.compile(r'"(FLOCK|T3CODE)_[A-Z0-9_]+"'),
        "include/flock/environment.hpp",
        "name environment variables in flock/environment.hpp",
    ),
    (re.compile(r"\bINTO journal\b"), "src/database.cpp", "write journal entries with Database::writeJournal"),
    (re.compile(r"\bvoid check\(bool\b"), "tests/support.hpp", "use check from tests/support.hpp"),
)
SCRIPT_OWNERS = (
    (re.compile(r"\bfunction (node|element)\(tag\b"), "web/assets/dom.js", "create elements with node from dom.js"),
    (re.compile(r"""^import [^;]*'/""", re.MULTILINE), None, "import page scripts by relative path, as in './dom.js'"),
)

STORAGE_MEMBER = re.compile(r"^\s+\w+\s+(\w+)\{([^;]*)\};", re.MULTILINE)


def storage_cycles(text):
    wiring = {name: re.findall(r"\w+", arguments) for name, arguments in STORAGE_MEMBER.findall(text)}
    graph = {name: {argument for argument in arguments if argument in wiring} for name, arguments in wiring.items()}

    def reaches(start):
        seen, pending = set(), [start]
        while pending:
            for target in graph[pending.pop()]:
                if target not in seen:
                    seen.add(target)
                    pending.append(target)
        return seen

    reach = {name: reaches(name) for name in graph}
    groups = []
    for name in graph:
        group = {other for other in reach[name] if name in reach[other]}
        if group and group not in groups:
            groups.append(group)
    return groups


def storage_violations(root):
    storage = root / "src" / "storage.hpp"
    if not storage.is_file():
        return []
    return [
        f"src/storage.hpp: {', '.join(sorted(group))} refer to each other in a cycle; "
        "depend on a component that only reads or writes records instead"
        for group in storage_cycles(storage.read_text(errors="surrogateescape"))
    ]


def is_test(path):
    return "tests" in path.parts[:-1] or TEST_FILE.search(path.name) is not None


def layer_of(root, name, includer):
    if name.startswith("flock/") and (root / "include" / name).is_file():
        return "include"
    relative = (includer.parent / name).resolve()
    if relative.is_file() and relative.is_relative_to(root):
        return relative.relative_to(root).parts[0]
    for layer in ALLOWED:
        if (root / layer / name).is_file():
            return layer
    return None


def violations(root):
    root = root.resolve()
    found = storage_violations(root)
    for path in sorted((root / "web" / "assets").glob("*.*js")):
        relative = path.relative_to(root)
        text = path.read_text(errors="surrogateescape")
        for pattern, owner, advice in SCRIPT_OWNERS:
            if relative.as_posix() != owner and pattern.search(text):
                found.append(f"{relative}: {advice}" + (f"; only {owner} does this directly" if owner else ""))
    for layer in (*ALLOWED, "tests"):
        allowed = ALLOWED.get(layer)
        for path in sorted((root / layer).rglob("*")):
            relative = path.relative_to(root)
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            text = path.read_text(errors="surrogateescape")
            for pattern, owner, advice in SINGLE_OWNERS:
                if relative.as_posix() != owner and is_test(relative) == is_test(Path(owner)) and pattern.search(text):
                    found.append(f"{relative}: {advice}; only {owner} does this directly")
            if is_test(relative):
                continue
            for name in INCLUDE.findall(text):
                target = layer_of(root, name, path)
                if layer == "include" and not name.startswith("flock/") and target is not None:
                    found.append(f'{relative}: public headers include project headers as "flock/...", not "{name}"')
                elif target is not None and target not in allowed:
                    found.append(
                        f'{relative}: {layer}/ may not include "{name}" from {target}/; '
                        "share it through include/flock or keep the code in the layer that owns it"
                    )
    return found


def main():
    parser = argparse.ArgumentParser(
        description="Check that each layer includes only its own headers and the public headers in include/flock, "
        "and that each shared mechanism has one implementation."
    )
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    found = violations(args.source)
    if found:
        print("Architecture check failed", file=sys.stderr)
        for violation in found:
            print(f"  {violation}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
