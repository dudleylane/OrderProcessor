#!/usr/bin/env bash
# ctest-skips.sh: fail on a test that ctest counted as skipped, unless the caller expects it to skip (#563, #635).
#
# usage: scripts/ctest-skips.sh LOG [EXPECTED]
#   LOG       ctest's output, as `ctest ... | tee LOG` writes it
#   EXPECTED  an extended regular expression: a skipped test whose name it matches is expected, and passes
#
# ctest counts a skipped test as a pass. A GoogleTest case that calls GTEST_SKIP() skips, since gtest_discover_tests
# gives each test a skip pattern; so does every case of a suite whose SetUpTestSuite() records a failure (#374); and so
# does a shell test that exits 77 for want of a tool (SKIP_RETURN_CODE). So a runner without jq would have stopped
# testing the runner's hook and the tested-tree decision with every step green. This reads the list ctest prints at the
# end of a run, "The following tests did not run:", and fails naming each skip that EXPECTED doesn't match.
#
# CI's test steps run it after each ctest, expecting only the Postgres tests to skip where no database is up; the
# Postgres steps and the pre-push hook expect none.
#
# Exit codes: 0 no unexpected skip; 1 at least one, each named; 2 usage error, or LOG can't be read

set -uo pipefail

if (( $# < 1 || $# > 2 )) || [[ -z "$1" ]]; then
    echo "usage: $0 LOG [EXPECTED]" >&2
    exit 2
fi
log=$1
expected=${2:-}
if [[ ! -r "$log" ]]; then
    echo "ctest-skips: can't read $log" >&2
    exit 2
fi

# The list's lines read "<tab><number> - <name> (Skipped)"
mapfile -t skipped < <(sed -nE 's/^[[:space:]]*[0-9]+ - (.+) \(Skipped\)[[:space:]]*$/\1/p' "$log")
unexpected=()
allowed=0
for name in "${skipped[@]}"; do
    if [[ -n "$expected" ]] && grep -qE -- "$expected" <<< "$name"; then
        allowed=$(( allowed + 1 ))
        continue
    fi
    unexpected+=("$name")
done

if (( ${#unexpected[@]} > 0 )); then
    for name in "${unexpected[@]}"; do
        if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
            echo "::error::$name skipped, and isn't one of the tests expected to skip here"
        fi
        echo "ctest-skips: $name skipped, and isn't one of the tests expected to skip here" >&2
    done
    echo "ctest-skips: ${#unexpected[@]} unexpected skip(s); ctest counts a skip as a pass" >&2
    exit 1
fi
echo "ctest-skips: no unexpected skip (${allowed} expected)"
