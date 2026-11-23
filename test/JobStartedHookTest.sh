#!/usr/bin/env bash
# The runner's job-started hook (#143, #168, #567), against payloads like the ones the runner writes for a job. It must
# run a push, a pull request from this repository, the schedule and a run by hand, and refuse a pull request from a fork,
# anything Dependabot sent, and any other event, by its name. And it must run every event a workflow's `on:` names.
# usage: JobStartedHookTest.sh <scripts/runner/job-started.sh, or a runner's installed copy>
set -u
HOOK=$1
command -v jq >/dev/null || { echo "jq is missing, so the hook can't run here: skipped"; exit 77; }
work=$(mktemp -d)
trap 'rm -rf -- "${work:?}"' EXIT
# The hook reads only the payload the runner writes under its own directory
mkdir -p "$work/_work/_temp/_github_workflow"
install -m 755 "$HOOK" "$work/job-started.sh"
event="$work/_work/_temp/_github_workflow/event.json"
failures=0
expect() { # run|refuse event-name (- for none) description payload
    printf '%s' "$4" > "$event"
    if [[ "$2" == - ]]; then
        env -u GITHUB_EVENT_NAME GITHUB_EVENT_PATH="$event" bash -e "$work/job-started.sh" > "$work/out" 2>&1 < /dev/null
    else
        GITHUB_EVENT_PATH="$event" GITHUB_EVENT_NAME="$2" bash -e "$work/job-started.sh" > "$work/out" 2>&1 < /dev/null
    fi
    local status=$? got=run
    [[ $status -ne 0 ]] && got=refuse
    if [[ "$got" == "$1" ]]; then
        echo "ok: $3: $got"
    else
        echo "FAIL: $3: expected $1, got $got"
        cat "$work/out"
        failures=$((failures + 1))
    fi
}
this='"repo":{"full_name":"dudleylane/OrderProcessor"}'
fork='"repo":{"full_name":"someone/OrderProcessor"}'
expect run push "a push by the maintainer" '{"ref":"refs/heads/master","sender":{"login":"dudleylane","type":"User"}}'
expect run pull_request "a pull request from this repository" \
    "{\"pull_request\":{\"head\":{$this},\"base\":{$this}},\"sender\":{\"login\":\"dudleylane\",\"type\":\"User\"}}"
expect refuse pull_request "a pull request from a fork" \
    "{\"pull_request\":{\"head\":{$fork},\"base\":{$this}},\"sender\":{\"login\":\"someone\",\"type\":\"User\"}}"
expect refuse push "the push of Dependabot's branch" \
    '{"ref":"refs/heads/dependabot/npm_and_yarn/vite-7.0.1","sender":{"login":"dependabot[bot]","type":"Bot"}}'
expect refuse pull_request "Dependabot's pull request" \
    "{\"pull_request\":{\"head\":{$this},\"base\":{$this}},\"sender\":{\"login\":\"dependabot[bot]\",\"type\":\"Bot\"}}"
expect refuse push "a payload that isn't JSON" 'not json'
# By the event's name (#567): until then the hook ran any payload with no pull_request object
maintainer='"sender":{"login":"dudleylane","type":"User"}'
expect run schedule "the weekly schedule" '{"schedule":"17 6 * * 1","repository":{"full_name":"dudleylane/OrderProcessor"}}'
expect run workflow_dispatch "a run by hand" "{\"inputs\":null,\"ref\":\"refs/heads/master\",$maintainer}"
expect refuse pull_request_target "pull_request_target from this repository" \
    "{\"pull_request\":{\"head\":{$this},\"base\":{$this}},$maintainer}"
expect refuse pull_request_target "pull_request_target from a fork" \
    "{\"pull_request\":{\"head\":{$fork},\"base\":{$this}},\"sender\":{\"login\":\"someone\",\"type\":\"User\"}}"
expect refuse workflow_run "a workflow_run, whose payload has no pull_request object" \
    '{"workflow_run":{"event":"pull_request","head_repository":{"full_name":"someone/OrderProcessor"}},"sender":{"login":"someone","type":"User"}}'
expect refuse issue_comment "a comment on an issue" \
    '{"issue":{"number":1},"comment":{"body":"run it"},"sender":{"login":"someone","type":"User"}}'
expect refuse - "a job the runner names no event for" "{\"ref\":\"refs/heads/master\",$maintainer}"
expect refuse push "a push whose payload holds a pull request" \
    "{\"ref\":\"refs/heads/master\",\"pull_request\":{\"head\":{$fork},\"base\":{$this}},$maintainer}"

# Every event a workflow runs on, the hook runs (#567). Each workflow gives its events as keys two spaces in under an
# `on:` line of its own; one that gives them another way fails here, until this reads that form too.
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
shopt -s nullglob
workflows=("$root"/.github/workflows/*.yml "$root"/.github/workflows/*.yaml)
if ((${#workflows[@]} == 0)); then
    echo "FAIL: no workflow under $root/.github/workflows"
    failures=$((failures + 1))
fi
for workflow in "${workflows[@]}"; do
    name=${workflow#"$root"/}
    mapfile -t events < <(awk '/^on:$/ { on = 1; next } on && /^[^ #]/ { exit } on && /^  [a-z_]+:/ {
        sub(/^  /, ""); sub(/:.*/, ""); print }' "$workflow")
    if ((${#events[@]} == 0)); then
        echo "FAIL: $name gives no event as a key under an 'on:' line of its own"
        failures=$((failures + 1))
        continue
    fi
    for name_of_event in "${events[@]}"; do
        payload="{\"ref\":\"refs/heads/master\",$maintainer}"
        if [[ "$name_of_event" == pull_request ]]; then
            payload="{\"pull_request\":{\"head\":{$this},\"base\":{$this}},$maintainer}"
        fi
        expect run "$name_of_event" "$name runs on $name_of_event" "$payload"
    done
done
exit $((failures > 0))
