#!/usr/bin/env bash
# ci-jobs.sh: how many compilers, LTRANS jobs and tests a CI job runs at once, its share of the host (#738).
#
# usage: scripts/ci-jobs.sh   prints the share, and says on stderr where it came from
#
# This repository's runners run their jobs at once on one host (#736), and each job took every thread for itself: the
# four sanitizer jobs of #739's first run built with 32 compilers on 8 threads, and held the runners' slice over its
# memory limit until GitHub gave the jobs up (#740). So a job takes a share: CI_BUILD_JOBS, which each runner's .env
# gives as the host's threads divided by its runners (the install notes in scripts/runner/job-started.sh). Where it
# isn't set, or is empty, the share is every thread, as on a host with one runner, and as before #738.
#
# Exit codes: 0 printed; 2 it refuses, with nothing on stdout: CI_BUILD_JOBS is something other than a positive
# number, or the host's threads can't be counted.

set -uo pipefail

threads=$(nproc) && [[ "$threads" =~ ^[1-9][0-9]*$ ]] || { echo "ci-jobs: can't count the host's threads" >&2; exit 2; }
share=${CI_BUILD_JOBS:-}
if [[ -z "$share" ]]; then
    echo "ci-jobs: all $threads of the host's threads: CI_BUILD_JOBS isn't set" >&2
    echo "$threads"
    exit 0
fi
[[ "$share" =~ ^[1-9][0-9]*$ ]] || { echo "ci-jobs: CI_BUILD_JOBS is '$share', not a positive number" >&2; exit 2; }
echo "ci-jobs: $share of the host's $threads threads, from CI_BUILD_JOBS" >&2
echo "$share"
