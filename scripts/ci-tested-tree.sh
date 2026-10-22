#!/usr/bin/env bash
# ci-tested-tree.sh: each workflow's first job, "Tested tree" (#314). It records the tree its run tests, and on a push
# it decides whether the workflow's other jobs run.
#
# Pull requests are rebase-merged, so the push of a merge carries a tree that the pull request's own run of the same
# workflow has already tested. Running every job again on it takes this repository's one runner for a full run and
# checks nothing new. So:
#   - Every run records the tree it checked out: a notice annotation on this job, titled "Tested tree". A pull
#     request's run checks out GitHub's merge of the pull request into its base, so it records that merge's tree, the
#     one it tested.
#   - A push skips the other jobs (run=false) only when a pull request merged into this branch has a run of this
#     workflow in which every job passed, and that run recorded this push's tree. The log and a notice name that run:
#     it is the push's CI record.
#   - A re-run of only some of a run's jobs (its failed jobs, or one job) makes an attempt that lists the jobs it
#     didn't run as copies of the previous attempt's, and a copy of this job has no record (#389). Every attempt of a
#     run checks out the run's own commit, so when the latest attempt's Tested tree has no record, the record is the
#     one the newest earlier attempt's Tested tree made. A latest attempt that recorded a tree is taken at its word.
#   - GitHub's answers settle some seconds after a merge, and a push's first job can ask before then: it linked a
#     rebase merge's commit to its pull request late (#331), and once answered without a pull request's passing run,
#     or its record, until later (#357). So until a passing run that recorded this tree turns up, the whole lookup is
#     asked again, five asks in all, six seconds apart, before the push runs everything. Each ask that finds none
#     says what it found. TESTED_TREE_RETRY_DELAY sets the seconds between asks (the test sets 0).
#   - Anything else runs them (run=true): any other event, a failed call listing the commit's pull requests, and,
#     after the five asks, no such pull request, a run that didn't pass, a different tree, no record in any attempt
#     or another failed call. The reason says what the last ask found.
#
# It writes run=true or run=false to $GITHUB_OUTPUT and exits 0. The workflows run every job when this one fails, so a
# failure here costs runner time, never coverage.
#
# usage: ci-tested-tree.sh <workflow file, e.g. build-test.yml>
# It reads GITHUB_OUTPUT, GITHUB_EVENT_NAME, GITHUB_SHA, GITHUB_REF_NAME and GITHUB_REPOSITORY, which the runner sets,
# and GH_TOKEN, the job's token. gh gets a configuration directory of its own: the runner runs as the maintainer, and gh
# must never fall back to the maintainer's login.
set -uo pipefail

workflow=${1:-}
[[ -n "$workflow" ]] || { echo "usage: ci-tested-tree.sh <workflow file>" >&2; exit 2; }
out=${GITHUB_OUTPUT:-}
[[ -n "$out" ]] || { echo "ci-tested-tree.sh: GITHUB_OUTPUT is not set" >&2; exit 2; }

decide() { # true|false reason
    echo "run=$1" >> "$out"
    [[ "$1" == false ]] && echo "::notice title=Already tested::$2"
    echo "Tested tree (#314): run=$1: $2"
    exit 0
}

tree=$(git rev-parse --verify --quiet 'HEAD^{tree}') || decide true "can't read the tree of this checkout"
echo "::notice title=Tested tree::$tree"

event=${GITHUB_EVENT_NAME:-}
[[ "$event" == push ]] || decide true "a ${event:-run with no event} tests what it checked out"
repo=${GITHUB_REPOSITORY:-}; sha=${GITHUB_SHA:-}; branch=${GITHUB_REF_NAME:-}
[[ -n "$repo" && -n "$sha" && -n "$branch" ]] \
    || decide true "GITHUB_REPOSITORY, GITHUB_SHA or GITHUB_REF_NAME is not set"
[[ -n "${GH_TOKEN:-}" ]] || decide true "GH_TOKEN is not set, so the pull request's run can't be looked up"
if ! command -v gh > /dev/null || ! command -v jq > /dev/null; then
    decide true "gh or jq is missing"
fi
GH_CONFIG_DIR=$(mktemp -d) || decide true "can't make a configuration directory for gh"
export GH_CONFIG_DIR GH_PROMPT_DISABLED=1 GH_NO_UPDATE_NOTIFIER=1
trap 'rm -rf -- "${GH_CONFIG_DIR:?}"' EXIT

asks=5; delay=${TESTED_TREE_RETRY_DELAY:-6}
[[ "$delay" =~ ^[0-9]+$ ]] || delay=6

seen=() # what this ask found in each merged pull request's runs, for the reason when none passed on this tree
# Decides run=false, for the reason given, if the job recorded this tree. Otherwise it adds what the job recorded to the
# caller's $recorded, or the job to the caller's $unread when its annotations can't be read.
check_record() { # job reason
    local annotations
    annotations=$(gh api "repos/$repo/check-runs/$1/annotations?per_page=100") || { unread+=" $1"; return; }
    if jq -e --arg tree "$tree" 'any(.[]; .title == "Tested tree" and .message == $tree)' <<< "$annotations" \
        > /dev/null; then
        decide false "$2"
    fi
    recorded+=$(jq -r '[.[] | select(.title == "Tested tree") | " " + .message] | join("")' <<< "$annotations")
}
# A run whose latest attempt's Tested tree has no record: a copy, when that attempt re-ran only some jobs (#389). Reads
# the earlier attempts' Tested tree jobs, newest first, past the copies: the first record decides run=false if it is
# this tree, and otherwise the caller's $note says what was found.
earlier() { # number id url attempt
    local number=$1 id=$2 url=$3 attempt=$4 all tested n job why
    all=$(gh api "repos/$repo/actions/runs/$id/jobs?per_page=100&filter=all") \
        || { note+=" in attempt $attempt, and its earlier attempts' jobs can't be listed"; return; }
    tested=$(jq -r --argjson latest "$attempt" '[.jobs[]
        | select(.name == "Tested tree" and .conclusion == "success" and .run_attempt < $latest)]
        | sort_by(-.run_attempt) | .[] | "\(.run_attempt) \(.id)"' <<< "$all") || tested=''
    note+=" in any of its $attempt attempts"
    while read -r n job; do
        [[ -n "$job" ]] || continue
        why="pull request #$number's run $url passed on this tree, $tree, as its attempt $n recorded"
        check_record "$job" "$why (attempt $attempt re-ran only some of its jobs)"
        [[ -n "$recorded$unread" ]] || continue
        note="#$number's run $id recorded no tree in attempt $attempt, and "
        if [[ -n "$unread" ]]; then
            note+="job $job's annotations (attempt $n) can't be read"
        else
            note+="attempt $n recorded$recorded"
        fi
        return
    done <<< "$tested"
}
look() { # number head: decides run=false if one of the pull request's runs of this workflow passed on this tree
    local number=$1 head=$2 runs listed id url jobs failed job recorded unread note attempt
    local query="head_sha=$head&event=pull_request&status=success&per_page=100"
    runs=$(gh api "repos/$repo/actions/workflows/$workflow/runs?$query") \
        || { seen+=("#$number: its runs can't be listed"); return; }
    listed=$(jq -r --arg head "$head" '.workflow_runs[]
        | select(.event == "pull_request" and .conclusion == "success" and .head_sha == $head)
        | "\(.id) \(.html_url)"' <<< "$runs") || listed=''
    [[ -n "$listed" ]] || { seen+=("#$number: no passing pull_request run listed"); return; }
    while read -r id url; do
        jobs=$(gh api "repos/$repo/actions/runs/$id/jobs?per_page=100") \
            || { seen+=("#$number's run $id: its jobs can't be listed"); continue; }
        # Every job ran and passed: a skipped one would make a run green without testing anything
        failed=$(jq -r 'if (.jobs | length) == 0 then "no jobs" else
            [.jobs[] | select(.conclusion != "success") | "\(.name) \(.conclusion)"] | join(", ") end' <<< "$jobs") \
            || failed="its jobs can't be read"
        [[ -z "$failed" ]] || { seen+=("#$number's run $id: $failed"); continue; }
        recorded='' unread=''
        for job in $(jq -r '.jobs[] | select(.name == "Tested tree") | .id' <<< "$jobs"); do
            check_record "$job" "pull request #$number's run $url passed on this tree, $tree"
        done
        note="#$number's run $id recorded${recorded:- no tree}"
        [[ -z "$unread" ]] || note+=", and job$unread's annotations can't be read"
        attempt=$(jq -r '[.jobs[].run_attempt] | max // 1' <<< "$jobs") || attempt=1
        if [[ -z "$recorded$unread" && "$attempt" =~ ^[1-9][0-9]*$ ]] && (( attempt > 1 )); then
            earlier "$number" "$id" "$url" "$attempt"
        fi
        seen+=("$note")
    done <<< "$listed"
}

for (( ask = 1; ; ask++ )); do
    pulls=$(gh api "repos/$repo/commits/$sha/pulls") || decide true "can't list the pull requests that have $sha"
    merged=$(jq -r --arg branch "$branch" \
        '.[] | select(.merged_at != null and .base.ref == $branch) | "\(.number) \(.head.sha)"' <<< "$pulls") \
        || decide true "can't read the pull requests that have $sha"
    if [[ -z "$merged" ]]; then
        waiting="No pull request merged into $branch has $sha yet"
        reason="no pull request merged into $branch has $sha, asked $asks times, $delay s apart"
    else
        seen=()
        while read -r number head; do look "$number" "$head"; done <<< "$merged"
        found=$(printf '%s; ' "${seen[@]}"); found=${found%; }
        waiting="No passing run of $workflow, in a pull request merged into $branch, has recorded this tree yet"
        waiting+=" ($found)"
        reason="no passing run of $workflow, in a pull request merged into $branch, recorded this tree, $tree, asked"
        reason+=" $asks times, $delay s apart ($found)"
    fi
    (( ask < asks )) || decide true "$reason"
    echo "$waiting (ask $ask of $asks): asking again in $delay s"
    sleep "$delay"
done
