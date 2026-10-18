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
#   - Anything else runs them (run=true): any other event, no such pull request, a run that didn't pass, a different
#     tree, a missing record, a failed API call.
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

pulls=$(gh api "repos/$repo/commits/$sha/pulls") || decide true "can't list the pull requests that have $sha"
merged=$(jq -r --arg branch "$branch" \
    '.[] | select(.merged_at != null and .base.ref == $branch) | "\(.number) \(.head.sha)"' <<< "$pulls") \
    || decide true "can't read the pull requests that have $sha"
[[ -n "$merged" ]] || decide true "no pull request merged into $branch has $sha"

while read -r number head; do
    query="head_sha=$head&event=pull_request&status=success&per_page=100"
    runs=$(gh api "repos/$repo/actions/workflows/$workflow/runs?$query") || continue
    while read -r id url; do
        jobs=$(gh api "repos/$repo/actions/runs/$id/jobs?per_page=100") || continue
        # Every job ran and passed: a skipped one would make a run green without testing anything
        jq -e '(.jobs | length) > 0 and all(.jobs[]; .conclusion == "success")' <<< "$jobs" > /dev/null || continue
        for job in $(jq -r '.jobs[] | select(.name == "Tested tree") | .id' <<< "$jobs"); do
            annotations=$(gh api "repos/$repo/check-runs/$job/annotations?per_page=100") || continue
            if jq -e --arg tree "$tree" 'any(.[]; .title == "Tested tree" and .message == $tree)' <<< "$annotations" \
                > /dev/null; then
                decide false "pull request #$number's run $url passed on this tree, $tree"
            fi
        done
    done < <(jq -r --arg head "$head" '.workflow_runs[]
        | select(.event == "pull_request" and .conclusion == "success" and .head_sha == $head)
        | "\(.id) \(.html_url)"' <<< "$runs")
done <<< "$merged"
decide true "no passing run of $workflow, in a pull request merged into $branch, recorded this tree, $tree"
