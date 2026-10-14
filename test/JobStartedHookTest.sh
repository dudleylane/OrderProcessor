#!/usr/bin/env bash
# The runner's job-started hook (#143, #168), against payloads like the ones the runner writes for a job. It must run a
# push, and a pull request from this repository, and refuse a pull request from a fork and anything Dependabot sent.
# usage: JobStartedHookTest.sh <scripts/runner/job-started.sh>
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
expect() { # run|refuse event-name description payload
    printf '%s' "$4" > "$event"
    GITHUB_EVENT_PATH="$event" GITHUB_EVENT_NAME="$2" bash "$work/job-started.sh" > "$work/out" 2>&1
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
exit $((failures > 0))
