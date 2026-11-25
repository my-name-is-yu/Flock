#!/bin/sh
set -eu

keep_build=false
if [ "${1:-}" = "--keep-build" ]; then
    keep_build=true
    shift
fi
if [ "$#" -ne 0 ]; then
    printf 'Usage: %s [--keep-build]\n' "$0" >&2
    exit 2
fi

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
"$source_dir/scripts/format.sh" --check

temp_root=$(CDPATH= cd -- "${TMPDIR:-/tmp}" && pwd -P)
build_dir=$(mktemp -d "$temp_root/flock-verify.XXXXXXXX")

cleanup() {
    status=$?
    trap - EXIT
    if [ "$keep_build" = true ]; then
        printf 'Kept verification build at %s\n' "$build_dir" >&2
    elif ! rm -rf -- "$build_dir"; then
        printf 'Could not remove verification build at %s\n' "$build_dir" >&2
        if [ "$status" -eq 0 ]; then
            status=1
        fi
    fi
    exit "$status"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

cmake --preset dev -S "$source_dir" -B "$build_dir"
cmake --build "$build_dir" --config Debug
ctest --test-dir "$build_dir" --build-config Debug --output-on-failure --no-tests=error
