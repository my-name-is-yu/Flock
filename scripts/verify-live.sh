#!/bin/sh
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
cd "$source_dir"

if [ "$#" -ne 0 ]; then
    printf 'Usage: %s\n' "$0" >&2
    exit 2
fi

build_dir="$source_dir/build-live"
cmake -S "$source_dir" -B "$build_dir" -DBUILD_TESTING=ON -DFLOCK_LIVE_TESTS=ON
cmake --build "$build_dir" --config Debug --target flock_live_t3 flock_live_conversation

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
