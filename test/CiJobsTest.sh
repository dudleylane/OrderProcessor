#!/usr/bin/env bash
# shellcheck disable=SC2016 # it greps for $jobs and ${JOBS:-$(nproc)} as text, so they stay in single quotes
# Each CI job's share of the host's threads (#738). The runners run their jobs at once on one host (#736), so a job
# builds, links and tests with a share, CI_BUILD_JOBS from its runner's .env, and with every thread only where that
# isn't set. scripts/ci-jobs.sh prints the share, or refuses one that isn't a positive number. Then the workflows, the
# server image and the Release links are read as they stand: a step, an image build or a link that took every thread
# again fails here. The pre-push hook isn't read: it builds alone, with every thread.
# usage: CiJobsTest.sh <the source tree>
set -u
ROOT=$(realpath -e -- "${1:-}") || { echo "no source tree '${1:-}'"; exit 1; }
SCRIPT=$ROOT/scripts/ci-jobs.sh
failures=0

check() { # <what> <expected> <actual>
    if [[ "$2" == "$3" ]]; then
        echo "ok: $1"
    else
        echo "FAILED: $1: expected '$2', got '$3'"
        failures=$(( failures + 1 ))
    fi
}

threads=$(nproc)

# The share, or every thread where CI_BUILD_JOBS is unset or empty, as on a host with one runner; and on stderr, which
# a job's log shows, where it came from
check "unset, every thread" "$threads" "$(env -u CI_BUILD_JOBS bash "$SCRIPT" 2> /dev/null)"
check "empty, every thread" "$threads" "$(CI_BUILD_JOBS='' bash "$SCRIPT" 2> /dev/null)"
check "a share" 2 "$(CI_BUILD_JOBS=2 bash "$SCRIPT" 2> /dev/null)"
check "a share above the host's threads" 64 "$(CI_BUILD_JOBS=64 bash "$SCRIPT" 2> /dev/null)"
check "it says where the share came from" "ci-jobs: 2 of the host's $threads threads, from CI_BUILD_JOBS" \
    "$(CI_BUILD_JOBS=2 bash "$SCRIPT" 2>&1 > /dev/null)"
check "and where every thread did" "ci-jobs: all $threads of the host's threads: CI_BUILD_JOBS isn't set" \
    "$(env -u CI_BUILD_JOBS bash "$SCRIPT" 2>&1 > /dev/null)"

# Anything else is refused, with nothing on stdout for a build to take
for share in 0 00 -1 two 1.5 02 ' 2' '2 ' '2 3' '$(nproc)'; do
    out=$(CI_BUILD_JOBS="$share" bash "$SCRIPT" 2> /dev/null)
    check "CI_BUILD_JOBS '$share' is refused" 2 $?
    check "with nothing on stdout" "" "$out"
done

cd "$ROOT" || exit 1
workflows=(.github/workflows/*.yml)

# Every workflow build takes the share: none counts the host's threads itself, and none runs cmake --build without it,
# since Ninja's default is every thread and two more
check "the workflows build" yes "$(grep -q 'cmake --build' "${workflows[@]}" && echo yes)"
check "no workflow counts the host's threads" "" "$(grep -n 'nproc' "${workflows[@]}")"
check "every workflow build takes the share" "" "$(grep -n 'cmake --build' "${workflows[@]}" | grep -vF -- '-j"$jobs"')"
# A step that took $jobs without asking the script for it would build with -j"", which is every thread too
check "every step that takes the share asks the script for it" "" "$(awk '
    function end_step() { if (takes && !asks) print FILENAME ": " step; takes = asks = 0 }
    /^ *- (name|uses):/ { end_step(); step = $0 }
    /"\$jobs"/ { takes = 1 }
    /^ *jobs=\$\(scripts\/ci-jobs\.sh\)/ { asks = 1 }
    ENDFILE { end_step() }
' "${workflows[@]}")"
# QuickFIX's builds take it too: scripts/build-quickfix.sh takes every thread unless it is given JOBS, which reaches it
# through memory-cap.sh's scope where the build runs under its cap (#740)
quickfix=$(grep -n 'build-quickfix\.sh "' "${workflows[@]}")
check "the workflows build QuickFIX" yes "$([[ -n "$quickfix" ]] && echo yes)"
check "and each build of it takes the share" "" \
    "$(grep -vE 'JOBS="\$jobs" (scripts/memory-cap\.sh [^ ]+ )?scripts/build-quickfix\.sh' <<< "$quickfix")"
# Every Release configure gives its links' LTRANS jobs the share
releases=$(cat "${workflows[@]}" | grep -c -- '-DCMAKE_BUILD_TYPE=Release')
check "the workflows configure a Release build" yes "$( (( releases > 0 )) && echo yes)"
check "and each gives its links the share" "$releases" "$(cat "${workflows[@]}" | grep -c -- '-DLTO_JOBS="$jobs"')"

# The server image's builds take its JOBS, and every thread only where JOBS is empty, as by default
dockerfile=$(tr -d '\r' < docker/Dockerfile.oms-server)
check "the image declares JOBS, empty by default" 1 "$(grep -c '^ARG JOBS=$' <<< "$dockerfile")"
check "it counts the threads only where JOBS is empty" "" \
    "$(grep -n 'nproc' <<< "$dockerfile" | grep -vF '"${JOBS:-$(nproc)}"')"
check "every build in it takes JOBS" "" \
    "$(grep -n 'cmake --build' <<< "$dockerfile" | grep -vF -- '-j"${JOBS:-$(nproc)}"')"
check "and its Release configure gives its links JOBS" 1 \
    "$(grep -cF -- '-DLTO_JOBS="${JOBS:-auto}"' <<< "$dockerfile")"
# A RUN that reads JOBS ahead of its ARG, or in another stage, reads it empty, and takes every thread: QuickFIX's
# build too, whose script reads JOBS from the environment
check "every build that reads JOBS follows its ARG, in its stage" "" "$(awk '
    /^FROM / { declared = 0 }
    /^ARG JOBS=$/ { declared = 1 }
    (/\$\{JOBS:-/ || /build-quickfix\.sh "/) && !declared { print NR ": " $0 }
' <<< "$dockerfile")"

# Every Release link's LTRANS jobs take LTO_JOBS: no -flto in the project's CMake files takes every thread on its own
flto=$(find . -name CMakeLists.txt -not -path '*/_deps/*' -not -path '*/node_modules/*' -print0 |
    xargs -0 -r grep -Hn -- '-flto' | tr -d '\r' | sed 's/#.*//' | grep -- '-flto')
check "the Release flags take LTO_JOBS" 2 "$(grep -cF -- '-flto=${LTO_JOBS}' <<< "$flto")"
check "and no other -flto is there" "" "$(grep -vF -- '-flto=${LTO_JOBS}' <<< "$flto")"

(( failures == 0 )) || { echo "$failures failed"; exit 1; }
echo "all passed"
