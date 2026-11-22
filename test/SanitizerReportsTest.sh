#!/usr/bin/env bash
# SanitizerReportsTest.sh: the sanitizer report check's own test (#672). A log with none of the runtimes' report lines
# passes; a log with any one fails, alone or beside a clean one; a missing log, or none, is a usage error.
#
# It prints no report line itself: ctest's LastTest.log keeps every test's output, and CI's check reads that log. So
# each line is put together as it is written to its file, and the check's own output, which names what it found, goes
# to a file this test reads only for its exit code.
#
# usage: test/SanitizerReportsTest.sh SCRIPT

set -uo pipefail

script=${1:?usage: SanitizerReportsTest.sh SCRIPT}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0

expect() { # <exit code> <what> [LOG...]
    local want=$1 what=$2 got=0
    shift 2
    bash "$script" "$@" > "$work/out" 2>&1 || got=$?
    if (( got == want )); then
        echo "ok: $what (exit $got)"
    else
        echo "FAILED: $what: exit $got, not $want"
        failures=$((failures + 1))
    fi
}

printf 'test output\n100%% tests passed\n' > "$work/clean.log"
expect 0 "a log with no report" "$work/clean.log"

# Each runtime's line, in two pieces, so that neither this file's text nor its output holds one whole
lines=(
    "x.cpp:1:2: runtime|error: signed integer overflow"
    "==1==ERROR: Address|Sanitizer: heap-use-after-free"
    "==1==ERROR: Leak|Sanitizer: detected memory leaks"
    "WARNING: Thread|Sanitizer: data race"
)
for i in "${!lines[@]}"; do
    first=${lines[$i]%%|*}
    second=${lines[$i]#*|}
    if [[ "$first" == *"runtime" ]]; then
        printf 'before\n%s %s\nafter\n' "$first" "$second" > "$work/report$i.log"
    else
        printf 'before\n%s%s\nafter\n' "$first" "$second" > "$work/report$i.log"
    fi
    expect 1 "a log with the report line of kind $((i + 1)) of 4" "$work/report$i.log"
done
expect 1 "a clean log beside one with a report" "$work/clean.log" "$work/report0.log"

# A NUL and a raw byte ahead of a report line: the check still finds the line, and names it, where grep alone would call
# the log a binary file
printf 'before\000\377\n' > "$work/binary.log"
printf '%s%s\nafter\n' "==1==ERROR: Address" "Sanitizer: heap-use-after-free" >> "$work/binary.log"
expect 1 "a log with raw bytes ahead of a report line" "$work/binary.log"
if grep -qi "binary file" "$work/out"; then
    echo "FAILED: the check called the log a binary file rather than naming its line"
    failures=$((failures + 1))
else
    echo "ok: the check names the line in a log with raw bytes"
fi
expect 2 "a log that isn't there" "$work/absent.log"
expect 2 "no log at all"

if (( failures > 0 )); then
    echo "$failures case(s) failed"
    exit 1
fi
echo "every case passed"
