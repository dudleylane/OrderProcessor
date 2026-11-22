#!/usr/bin/env bash
# scripts/line-length.sh (#615), against a made-up tree: a line of C++ longer than .clang-format's ColumnLimit fails,
# named with its file and line; the CR of a CRLF line doesn't count, a character outside ASCII counts once, and files
# outside src, test, app and bench, or not .h and .cpp, aren't read. A tree without a ColumnLimit is refused.
# usage: LineLengthTest.sh <scripts/line-length.sh>
set -u
SCRIPT=$(realpath -e -- "$1") || { echo "no script $1"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf -- "${work:?}"' EXIT
failures=0

check() { # <what> <expected> <actual>
    if [[ "$2" == "$3" ]]; then
        echo "ok: $1"
    else
        echo "FAILED: $1: expected '$2', got '$3'"
        failures=$(( failures + 1 ))
    fi
}

# A line of exactly n characters: "//" and n - 2 of the given character
line() { # <n> [character]
    local ch=${2:-x} body=""
    while (( ${#body} < $1 - 2 )); do body+=$ch; done
    printf '//%s' "$body"
}

tree=$work/tree
mkdir -p "$tree/src" "$tree/test" "$tree/app" "$tree/bench" "$tree/docs"
printf 'ColumnLimit: 20\nReflowComments: false\n' > "$tree/.clang-format"
{ line 20; printf '\n'; } > "$tree/src/AtLimit.h"
{ line 20; printf '\r\n'; line 20; printf '\r\n'; } > "$tree/test/Crlf.cpp"
{ LC_ALL=C.UTF-8 line 20 '—'; printf '\n'; } > "$tree/app/Dashes.cpp"
{ line 40; printf '\n'; } > "$tree/docs/Long.cpp"
{ line 40; printf '\n'; } > "$tree/bench/Long.txt"

out=$(bash "$SCRIPT" "$tree" 2>&1)
check "a tree within the limit passes" "0" "$?"

{ line 20; printf '\n'; line 21; printf '\n'; } > "$tree/bench/Over.cpp"
{ line 21; printf '\r\n'; } > "$tree/src/OverCrlf.h"
{ LC_ALL=C.UTF-8 line 21 '—'; printf '\n'; } > "$tree/app/OverDashes.h"
out=$(bash "$SCRIPT" "$tree" 2>&1)
check "a line over the limit fails" "1" "$?"
check "names the long line with its file and line" "1" "$(grep -c '^bench/Over.cpp:2: 21 characters' <<< "$out")"
check "a CRLF line's CR doesn't count, but its other 21 do" "1" "$(grep -c '^src/OverCrlf.h:1: 21 characters' <<< "$out")"
check "a character outside ASCII counts once" "1" "$(grep -c '^app/OverDashes.h:1: 21 characters' <<< "$out")"
check "names nothing at the limit" "0" "$(grep -cE 'AtLimit|Crlf.cpp|/Dashes.cpp' <<< "$out")"
check "reads only .h and .cpp under src, test, app and bench" "0" "$(grep -cE 'docs/|Long.txt' <<< "$out")"
check "counts the lines over" "1" "$(grep -c '3 line(s) over 20 columns' <<< "$out")"

printf 'ReflowComments: false\n' > "$tree/.clang-format"
out=$(bash "$SCRIPT" "$tree" 2>&1)
check "a tree without a ColumnLimit is refused" "2" "$?"

if (( failures > 0 )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
