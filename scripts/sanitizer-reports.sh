#!/usr/bin/env bash
# sanitizer-reports.sh: fail on a sanitizer's report in a test log, whether or not the report halted its test (#672).
#
# usage: scripts/sanitizer-reports.sh LOG...
#   LOG  a log the runtimes' reports may be in: ctest's Testing/Temporary/LastTest.log holds every test's output
#
# A report that halts its test fails it, and ctest with it. One that doesn't halt lets its test pass: libubsan reads
# UBSAN_OPTIONS once, as it makes its first report, and a test that starved the process of descriptors there left it at
# its defaults, which don't halt (#672). CI's sanitizer jobs run this after each ctest, so such a report fails the job
# all the same. It looks for the line each runtime starts a report with: UBSan's "runtime error:", ASan's and LSan's
# "ERROR: AddressSanitizer" and "ERROR: LeakSanitizer", and TSan's "WARNING: ThreadSanitizer". A test must not print
# one of them by design. A log is read as text whatever bytes a test wrote into it (grep -a): grep would otherwise call
# it a binary file and name no line.
#
# Exit codes: 0 no report; 1 at least one, each log's count named, with the first report's line; 2 usage error, or a
# LOG can't be read

set -uo pipefail

if (( $# < 1 )); then
    echo "usage: $0 LOG..." >&2
    exit 2
fi
for log in "$@"; do
    if [[ ! -r "$log" ]]; then
        echo "sanitizer-reports: can't read $log" >&2
        exit 2
    fi
done

reports='runtime error:|ERROR: AddressSanitizer|ERROR: LeakSanitizer|WARNING: ThreadSanitizer'
found=0
for log in "$@"; do
    count=$(grep -a -c -E "$reports" "$log")
    if (( count > 0 )); then
        echo "sanitizer-reports: $log holds $count sanitizer report line(s), the first:" >&2
        grep -a -m 1 -E "$reports" "$log" >&2
        found=1
    fi
done
if (( found == 0 )); then
    echo "sanitizer-reports: no report in $*"
fi
exit "$found"
