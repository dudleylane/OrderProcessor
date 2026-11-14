#!/usr/bin/env bash
# Every build in CI's workflows runs under scripts/memory-cap.sh (#740), read as the workflows stand. The script runs a
# build's compilers in a scope with a cap, in the slice for the jobs that the runner's MEMORY_CAP_SLICE names, beside
# the runners' own slice. A build left outside it would compile in its runner's service: there it would throttle the
# runners as #739's builds did, and the runners' slice's limit, kept small for the runners, would kill it unnamed.
# usage: CiBuildsCappedTest.sh <the source tree>
set -u
ROOT=$(realpath -e -- "${1:-}") || { echo "no source tree '${1:-}'"; exit 1; }
failures=0

check() { # <what> <expected> <actual>
    if [[ "$2" == "$3" ]]; then
        echo "ok: $1"
    else
        echo "FAILED: $1: expected '$2', got '$3'"
        failures=$(( failures + 1 ))
    fi
}

cd "$ROOT" || exit 1
workflows=(.github/workflows/*.yml)

# Each line that builds, comments left out: cmake --build, and on feature/fx-swap-support scripts/build-quickfix.sh
builds=$(grep -n -E 'cmake --build|build-quickfix\.sh' "${workflows[@]}" | grep -v -E '^[^:]+:[0-9]+:[[:space:]]*#')
check "the workflows build" yes "$(grep -q 'cmake --build' <<< "$builds" && echo yes)"
check "every build runs under memory-cap.sh, with a size" "" \
    "$(grep -v -E 'scripts/memory-cap\.sh [1-9][0-9]*[KMGT] (cmake --build|scripts/build-quickfix\.sh) ' <<< "$builds")"

(( failures == 0 )) || { echo "$failures failed"; exit 1; }
echo "all passed"
