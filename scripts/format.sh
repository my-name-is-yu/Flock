#!/bin/bash
set -euo pipefail

if [ "$#" -eq 0 ]; then
    set -- -i
elif [ "$#" -eq 1 ] && [ "$1" = --check ]; then
    set -- --dry-run --Werror
else
    printf 'Usage: %s [--check]\n' "$0" >&2
    exit 2
fi

fail() {
    printf '%s\n' "$1" >&2
    exit 1
}

major=21
if [ -n "${FLOCK_CLANG_FORMAT:-}" ]; then
    formatter=$(type -P -- "$FLOCK_CLANG_FORMAT") || fail "FLOCK_CLANG_FORMAT is not executable: $FLOCK_CLANG_FORMAT"
elif ! formatter=$(xcrun --find clang-format 2>/dev/null) && ! formatter=$(type -P clang-format); then
    fail "clang-format $major is required. Install it or set FLOCK_CLANG_FORMAT to its path."
fi

case "$formatter" in
    /*) ;;
    *) formatter="$PWD/$formatter" ;;
esac

version=$("$formatter" --version) || fail "Could not read the clang-format version from $formatter."
case "$version" in
    *"clang-format version $major."*) ;;
    *) fail "Expected clang-format $major at $formatter, found: $version" ;;
esac

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
cd "$source_dir"
git ls-files -z -- '*.cpp' '*.hpp' \
    ':(exclude,glob)build*/**' ':(exclude,glob)**/build/**' \
    ':(exclude,glob)**/build-*/**' ':(exclude,glob)**/cmake-build-*/**' \
    ':(exclude,glob)**/vendor/**' ':(exclude,glob)**/third_party/**' ':(exclude,glob)**/third-party/**' |
    xargs -0 -r "$formatter" "$@" --style="file:$source_dir/.clang-format" --fail-on-incomplete-format --
