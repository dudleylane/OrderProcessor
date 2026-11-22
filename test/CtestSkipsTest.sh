#!/usr/bin/env bash
# scripts/ctest-skips.sh (#563, #635), against ctest output: a test ctest counted as skipped fails the check unless the
# expected pattern matches its name, and tests that ran, failed or were disabled aren't skips. A log that can't be read,
# or no log, is refused.
# usage: CtestSkipsTest.sh <scripts/ctest-skips.sh>
set -u
# A workflow sets GITHUB_ACTIONS, and the script then adds an annotation line for each skip, which the counts below
# would see twice: the cases run without it, and the annotation's case sets it itself
unset GITHUB_ACTIONS
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

# ctest's own words, as a run with two skips, a failure and a disabled test ends
cat > "$work/skips.log" << 'EOF'
        Start   1: PGWriteBehindTest.WritesAnOrder
  1/6 Test   #1: PGWriteBehindTest.WritesAnOrder .................***Skipped   0.01 sec
        Start   2: JobStartedHookDecides
  2/6 Test   #2: JobStartedHookDecides ...........................***Skipped   0.02 sec
        Start   3: OrderBookTest.Adds
  3/6 Test   #3: OrderBookTest.Adds ..............................   Passed    0.01 sec
        Start   4: OrderBookTest.Removes
  4/6 Test   #4: OrderBookTest.Removes ...........................***Failed    0.01 sec
  5/6 Test   #5: OrderBookTest.DISABLED_Old ......................***Not Run (Disabled)   0.00 sec
        Start   6: StorageRecordDispatcherTest.PostgresFollowsARollbackToTheVersionLmdbKeeps
  6/6 Test   #6: StorageRecordDispatcherTest.PostgresFollowsARollbackToTheVersionLmdbKeeps ...***Skipped   0.01 sec

67% tests passed, 1 tests failed out of 6

Total Test time (real) =   0.08 sec

The following tests did not run:
	  1 - PGWriteBehindTest.WritesAnOrder (Skipped)
	  2 - JobStartedHookDecides (Skipped)
	  5 - OrderBookTest.DISABLED_Old (Disabled)
	  6 - StorageRecordDispatcherTest.PostgresFollowsARollbackToTheVersionLmdbKeeps (Skipped)

The following tests FAILED:
	  4 - OrderBookTest.Removes (Failed)
EOF
cat > "$work/clean.log" << 'EOF'
  1/2 Test   #1: OrderBookTest.Adds ..............................   Passed    0.01 sec
  2/2 Test   #2: OrderBookTest.Removes ...........................   Passed    0.01 sec

100% tests passed, 0 tests failed out of 2
EOF

out=$(bash "$SCRIPT" "$work/clean.log" 2>&1)
check "a run with no skip passes" "0" "$?"

out=$(bash "$SCRIPT" "$work/skips.log" 2>&1)
check "a skip with nothing expected fails" "1" "$?"
check "names the shell test's skip" "1" "$(grep -c 'JobStartedHookDecides skipped' <<< "$out")"
check "names the Postgres skips too, when nothing is expected" "2" "$(grep -cE '(PGWriteBehindTest|Postgres).* skipped' <<< "$out")"
check "a failure isn't a skip" "0" "$(grep -c 'OrderBookTest.Removes' <<< "$out")"
check "a disabled test isn't a skip" "0" "$(grep -c 'DISABLED_Old' <<< "$out")"

out=$(bash "$SCRIPT" "$work/skips.log" 'PGWriteBehind|Postgres' 2>&1)
check "an unexpected skip beside expected ones fails" "1" "$?"
check "names only the unexpected one" "1" "$(grep -c ' skipped, and' <<< "$out")"
check "...which is the shell test" "1" "$(grep -c 'JobStartedHookDecides skipped' <<< "$out")"

grep -v 'JobStartedHookDecides' "$work/skips.log" > "$work/expected.log"
out=$(bash "$SCRIPT" "$work/expected.log" 'PGWriteBehind|Postgres' 2>&1)
check "only expected skips pass" "0" "$?"
check "and says how many were expected" "1" "$(grep -c '(2 expected)' <<< "$out")"

out=$(GITHUB_ACTIONS=true bash "$SCRIPT" "$work/skips.log" 'PGWriteBehind|Postgres' 2>&1)
check "in a workflow, an annotation names it" "1" "$(grep -c '^::error::JobStartedHookDecides skipped' <<< "$out")"

out=$(bash "$SCRIPT" "$work/missing.log" 2>&1)
check "a log that can't be read is refused" "2" "$?"
out=$(bash "$SCRIPT" 2>&1)
check "no log is refused" "2" "$?"

if (( failures > 0 )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
