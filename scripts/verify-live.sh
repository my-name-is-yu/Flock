#!/bin/sh
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
cd "$source_dir"

if [ "$#" -eq 1 ] && [ "$1" = --changed-only ]; then
    set -- t3/ ':(glob)app/model*' ':(glob)app/chatgpt*' src/conversation_agent.cpp \
        src/runtime.cpp include/flock/core.hpp ':(glob)tests/live_*'
    base=$(git merge-base origin/main HEAD)
    changed=$(git diff --name-only --no-renames "$base" -- "$@")
    if [ -z "$changed" ]; then
        changed=$(git ls-files --others --exclude-standard -- "$@")
    fi
    if [ -z "$changed" ]; then
        printf '%s\n' 'live gate not required'
        exit 0
    fi
elif [ "$#" -ne 0 ]; then
    printf 'Usage: %s [--changed-only]\n' "$0" >&2
    exit 2
fi

build_dir="$source_dir/build-live"
cmake -S "$source_dir" -B "$build_dir" -DBUILD_TESTING=ON -DFLOCK_LIVE_TESTS=ON
cmake --build "$build_dir" --config Debug

temp_root=$(CDPATH= cd -- "${TMPDIR:-/tmp}" && pwd -P)
report_dir=$(mktemp -d "$temp_root/flock-live.XXXXXXXX")
trap 'rm -r -- "$report_dir"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

report="$report_dir/live.xml"
ctest_status=0
ctest --test-dir "$build_dir" --build-config Debug -L '^live$' -j 1 \
    --output-on-failure --no-tests=error --output-junit "$report" || ctest_status=$?
python3 "$source_dir/scripts/check-live-junit.py" "$report" "$ctest_status"
