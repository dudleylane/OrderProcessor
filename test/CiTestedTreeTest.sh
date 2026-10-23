#!/usr/bin/env bash
# Each workflow's tested-tree job (#314), against canned API answers in the shapes GitHub's API returns. A push may skip
# the jobs only when a pull request merged into its branch has a run of the workflow in which every job passed and which
# recorded the push's tree; anything else, a failed call among them, must run them, after asking again (#331, #357,
# #392) and saying what it found.
# usage: CiTestedTreeTest.sh <scripts/ci-tested-tree.sh>
set -u
SCRIPT=$(realpath -e -- "$1") || { echo "no script $1"; exit 1; }
command -v jq > /dev/null || { echo "jq is missing, so the script can't run here: skipped"; exit 77; }
command -v git > /dev/null || { echo "git is missing: skipped"; exit 77; }
work=$(mktemp -d)
trap 'rm -rf -- "${work:?}"' EXIT

# A checkout: the script reads the tree of HEAD
git init -q "$work/repo"
echo tree > "$work/repo/file"
git -C "$work/repo" add file
git -C "$work/repo" -c user.name=test -c user.email=test@example.com commit -q -m tree
sha=$(git -C "$work/repo" rev-parse HEAD)
tree=$(git -C "$work/repo" rev-parse 'HEAD^{tree}')
other=0123456789abcdef0123456789abcdef01234567
repo=dudleylane/OrderProcessor
head=89abcdef0123456789abcdef0123456789abcdef
runs_endpoint="repos/$repo/actions/workflows/build-test.yml/runs"
runs_endpoint+="?head_sha=$head&event=pull_request&status=success&per_page=100"

# A gh that answers `gh api <endpoint>` from $FAKE_GH_DIR, and fails as GitHub's 404 does for an endpoint it has no
# answer for. It notes each call, and whether gh was given a token and a configuration directory of its own. An answer
# can change between calls: <answer>.<n>, where there is one, answers the endpoint's nth call, and <answer>.<n>.fail
# fails it, as GitHub's HTTP 500s did (#392).
mkdir "$work/bin"
cat > "$work/bin/gh" << 'EOF'
#!/usr/bin/env bash
if [[ -n "${GH_TOKEN:-}" && -d "${GH_CONFIG_DIR:-}" && "$GH_CONFIG_DIR" != "$HOME/.config/gh" ]]; then
    echo "call $*" >> "$FAKE_GH_LOG"
else
    echo "call with the wrong token or configuration: $*" >> "$FAKE_GH_LOG"
fi
[[ $# -eq 2 && "$1" == api ]] || { echo "gh: unexpected arguments: $*" >&2; exit 1; }
answer="$FAKE_GH_DIR/$(printf '%s' "$2" | tr -c 'A-Za-z0-9._-' '_')"
n=$(grep -cxF "call $*" "$FAKE_GH_LOG")
[[ -f "$answer.$n.fail" ]] && { echo "unexpected end of JSON input" >&2; exit 1; }
[[ -f "$answer.$n" ]] && answer="$answer.$n"
[[ -f "$answer" ]] || { echo "gh: Not Found (HTTP 404)" >&2; exit 1; }
cat "$answer"
EOF
chmod 755 "$work/bin/gh"

answer() { # endpoint json: the fake gh's answer for the endpoint
    printf '%s\n' "$2" > "$answers/$(printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_')"
}
answer_at() { # endpoint n json: the fake gh's answer for the endpoint's nth call only
    printf '%s\n' "$3" > "$answers/$(printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_').$2"
}
fail_at() { # endpoint n: the fake gh fails the endpoint's nth call, as GitHub's HTTP 500s did (#392)
    : > "$answers/$(printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_').$2.fail"
}
# The answers for a pull request merged into a base, whose run of build-test.yml recorded a tree
merged() { # base merged_at run_event run_conclusion job_conclusion annotation_title annotation_tree [number head run]
    local number=${8:-7} pr_head=${9:-$head} run=${10:-101}
    local query="head_sha=$pr_head&event=pull_request&status=success&per_page=100"
    answer "repos/$repo/commits/$sha/pulls" "$(jq -n --argjson number "$number" --argjson merged_at "$2" \
        --arg base "$1" --arg head "$pr_head" \
        '[{number: $number, state: "closed", merged_at: $merged_at, base: {ref: $base}, head: {sha: $head}}]')"
    answer "repos/$repo/actions/workflows/build-test.yml/runs?$query" "$(jq -n --argjson id "$run" --arg event "$3" \
        --arg conclusion "$4" --arg head "$pr_head" --arg url "https://github.com/$repo/actions/runs/$run" \
        '{total_count: 1, workflow_runs: [{id: $id, event: $event, status: "completed", conclusion: $conclusion,
          head_sha: $head, path: ".github/workflows/build-test.yml", run_attempt: 1, html_url: $url}]}')"
    answer "repos/$repo/actions/runs/$run/jobs?per_page=100" "$(jq -n --argjson id "$run" --arg conclusion "$5" \
        '{total_count: 2, jobs: [
          {id: ($id * 10 + 1), name: "Tested tree", run_attempt: 1, status: "completed", conclusion: "success"},
          {id: ($id * 10 + 2), name: "Release build + ctest", run_attempt: 1, status: "completed",
           conclusion: $conclusion}]}')"
    answer "repos/$repo/check-runs/${run}1/annotations?per_page=100" "$(jq -n --arg title "$6" --arg tree "$7" \
        '[{path: ".github", start_line: 1, end_line: 1, annotation_level: "notice", title: $title, message: $tree,
          raw_details: ""}]')"
}
# Then re-runs of that run's failed job alone, as `gh run rerun --failed` makes (#389): each attempt after the first
# lists the jobs it didn't run as copies of the previous attempt's, and a copy of Tested tree has no annotations.
# Attempt a's jobs are run * 10 + 2a - 1 (Tested tree) and run * 10 + 2a.
rerun_failed() { # attempts [run]
    local attempts=$1 run=${2:-101} a runs
    local endpoint="repos/$repo/actions/runs/$run/jobs?per_page=100"
    runs="$answers/$(printf '%s' "$runs_endpoint" | tr -c 'A-Za-z0-9._-' '_')"
    jq --argjson last "$attempts" '.workflow_runs[0].run_attempt = $last' "$runs" > "$runs.new" \
        && mv "$runs.new" "$runs"
    answer "$endpoint" "$(jq -n --argjson run "$run" --argjson last "$attempts" '{total_count: 2, jobs: [
        {id: ($run * 10 + 2 * $last - 1), name: "Tested tree", run_attempt: $last, status: "completed",
         conclusion: "success"},
        {id: ($run * 10 + 2 * $last), name: "Release build + ctest", run_attempt: $last, status: "completed",
         conclusion: "success"}]}')"
    answer "$endpoint&filter=all" "$(jq -n --argjson run "$run" --argjson last "$attempts" '{total_count: (2 * $last),
        jobs: [range(1; $last + 1) as $a
          | {id: ($run * 10 + 2 * $a - 1), name: "Tested tree", run_attempt: $a, status: "completed",
             conclusion: "success"},
            {id: ($run * 10 + 2 * $a), name: "Release build + ctest", run_attempt: $a, status: "completed",
             conclusion: (if $a == $last then "success" else "failure" end)}]}')"
    for (( a = 2; a <= attempts; a++ )); do
        answer "repos/$repo/check-runs/$((run * 10 + 2 * a - 1))/annotations?per_page=100" '[]'
    done
}
new_case() { answers=$(mktemp -d -p "$work"); : > "$work/gh.log"; }

failures=0
expect() { # true|false description event branch [token]
    rm -f "$work/output"
    (cd "$work/repo" && env -i PATH="$work/bin:/usr/bin:/bin" HOME="$work" GITHUB_OUTPUT="$work/output" \
        GITHUB_EVENT_NAME="$3" GITHUB_SHA="$sha" GITHUB_REF_NAME="$4" GITHUB_REPOSITORY="$repo" \
        GH_TOKEN="${5-token}" TESTED_TREE_RETRY_DELAY=0 FAKE_GH_DIR="$answers" FAKE_GH_LOG="$work/gh.log" \
        bash "$SCRIPT" build-test.yml) \
        > "$work/stdout" 2>&1
    local status=$? got
    got=$(cat "$work/output" 2>/dev/null)
    if [[ $status -ne 0 || "$got" != "run=$1" ]]; then
        echo "FAIL: $2: expected exit 0 and run=$1, got exit $status and '${got//$'\n'/ }'"
        cat "$work/stdout"
        failures=$((failures + 1))
    elif ! grep -qx "::notice title=Tested tree::$tree" "$work/stdout"; then
        echo "FAIL: $2: no 'Tested tree' notice for $tree"
        cat "$work/stdout"
        failures=$((failures + 1))
    elif grep -q 'wrong token' "$work/gh.log"; then
        echo "FAIL: $2: gh ran without the job's token, or with the user's configuration"
        cat "$work/gh.log"
        failures=$((failures + 1))
    else
        echo "ok: $2: run=$1"
    fi
}
calls() { wc -l < "$work/gh.log"; }
expect_asked() { # n: how many times the script asked for the commit's pull requests
    local n; n=$(grep -c "commits/$sha/pulls" "$work/gh.log")
    [[ $n -eq $1 ]] && return
    echo "FAIL: it asked for the commit's pull requests $n times, not $1"
    failures=$((failures + 1))
}
expect_said() { # text: the script's output has it
    grep -qF -- "$1" "$work/stdout" && return
    echo "FAIL: the output doesn't say: $1"
    cat "$work/stdout"
    failures=$((failures + 1))
}
expect_not_asked() { # text: no call to gh had it
    ! grep -qF -- "$1" "$work/gh.log" && return
    echo "FAIL: gh was asked for $1"
    cat "$work/gh.log"
    failures=$((failures + 1))
}

new_case
expect true "a pull request's run records its tree and runs" pull_request master
[[ $(calls) -eq 0 ]] || { echo "FAIL: a pull request's run called gh"; failures=$((failures + 1)); }
new_case
expect true "a scheduled run" schedule master
new_case
expect true "a run started by hand" workflow_dispatch master

new_case
merged master '"2026-10-18T08:28:00Z"' pull_request success success "Tested tree" "$tree"
expect false "a merge whose pull request's run passed on the same tree" push master
grep -q "::notice title=Already tested::pull request #7's run https://github.com/$repo/actions/runs/101" \
    "$work/stdout" || { echo "FAIL: the skip doesn't name the run it relied on"; failures=$((failures + 1)); }
new_case
merged feature/fx-swap-support '"2026-10-18T09:49:31Z"' pull_request success success "Tested tree" "$tree"
expect false "the same on the FX branch" push feature/fx-swap-support

new_case
merged master '"2026-10-18T08:28:00Z"' pull_request success success "Tested tree" "$other"
expect true "the pull request's run tested another tree" push master
expect_asked 5
expect_said "run=true: no passing run of build-test.yml, in a pull request merged into master, recorded this tree, \
$tree, asked 5 times, 0 s apart (#7's run 101 recorded $other)"
new_case
merged master '"2026-10-18T08:28:00Z"' pull_request success skipped "Tested tree" "$tree"
expect true "a job of the pull request's run was skipped" push master
expect_said "(#7's run 101: Release build + ctest skipped)"
new_case
merged master '"2026-10-18T08:28:00Z"' pull_request failure failure "Tested tree" "$tree"
expect true "the pull request's run failed" push master
expect_said "(#7: no passing pull_request run listed)"
new_case
merged master '"2026-10-18T08:28:00Z"' push success success "Tested tree" "$tree"
expect true "the only run is a push's, not the pull request's" push master
expect_said "(#7: no passing pull_request run listed)"
new_case
merged master '"2026-10-18T08:28:00Z"' pull_request success success "Already tested" "$tree"
expect true "the run has no 'Tested tree' record" push master
expect_said "(#7's run 101 recorded no tree)"
expect_not_asked "filter=all"
new_case
merged feature/fx-swap-support '"2026-10-18T09:49:31Z"' pull_request success success "Tested tree" "$tree"
expect true "the pull request was merged into the other branch" push master
new_case
merged master null pull_request success success "Tested tree" "$tree"
expect true "the pull request is still open" push master
new_case
answer "repos/$repo/commits/$sha/pulls" '[]'
expect true "a push no pull request has" push master
expect_asked 5
new_case
expect true "the pull requests can't be listed" push master
expect_asked 5
expect_said "run=true: can't list the pull requests that have $sha, asked 5 times, 0 s apart"
new_case
merged master '"2026-10-18T08:28:00Z"' pull_request success success "Tested tree" "$tree"
rm -f "$answers/repos_${repo//\//_}_check-runs_1011_annotations_per_page_100"
expect true "the run's annotations can't be read" push master
expect_asked 5
expect_said "(#7's run 101 recorded no tree, and job 1011's annotations can't be read)"
new_case
merged master '"2026-10-18T08:28:00Z"' pull_request success success "Tested tree" "$tree"
expect true "no token: gh isn't asked" push master ""
[[ $(calls) -eq 0 ]] || { echo "FAIL: gh ran with no token"; failures=$((failures + 1)); }

# GitHub links a merge's commit to its pull request seconds after the merge (#331): until then the commit lists none
new_case
merged master '"2026-10-18T22:17:51Z"' pull_request success success "Tested tree" "$tree"
pulls_answer="$answers/repos_${repo//\//_}_commits_${sha}_pulls"
echo '[]' > "$pulls_answer.1"
echo '[]' > "$pulls_answer.2"
expect false "the commit is linked to its pull request only at the third ask" push master
expect_asked 3

# Seconds after a merge GitHub once answered without the pull request's passing run (#357): its run, or its record,
# turns up at a later ask
new_case
merged master '"2026-10-19T16:07:16Z"' pull_request success success "Tested tree" "$tree"
answer_at "$runs_endpoint" 1 '{"total_count": 0, "workflow_runs": []}'
expect false "the pull request's run is listed only at the second ask" push master
expect_asked 2
expect_said "has recorded this tree yet (#7: no passing pull_request run listed) (ask 1 of 5): asking again in 0 s"
new_case
merged master '"2026-10-19T16:07:16Z"' pull_request success success "Tested tree" "$tree"
answer_at "repos/$repo/check-runs/1011/annotations?per_page=100" 1 '[]'
answer_at "repos/$repo/check-runs/1011/annotations?per_page=100" 2 '[]'
expect false "the run's record is listed only at the third ask" push master
expect_asked 3
expect_said "has recorded this tree yet (#7's run 101 recorded no tree) (ask 2 of 5)"

# For about two hours GitHub failed many calls listing a commit's pull requests (#392): a failed call, or an answer
# that can't be read, is asked again
new_case
merged master '"2026-10-22T17:27:44Z"' pull_request success success "Tested tree" "$tree"
fail_at "repos/$repo/commits/$sha/pulls" 1
expect false "the pull requests can be listed only at the second ask" push master
expect_asked 2
expect_said "Can't list the pull requests that have $sha (ask 1 of 5): asking again in 0 s"
new_case
answer "repos/$repo/commits/$sha/pulls" '{"message": "Server Error"}'
expect true "the pull requests' answer can't be read" push master
expect_asked 5
expect_said "run=true: can't read the pull requests that have $sha, asked 5 times, 0 s apart"

# A re-run of a run's failed job alone lists Tested tree as a copy with no record (#389). Every attempt checks out the
# run's commit, so the record is the one the newest earlier attempt's Tested tree made
new_case
merged master '"2026-10-22T07:49:02Z"' pull_request success success "Tested tree" "$tree"
rerun_failed 2
expect false "the pull request's run passed on a re-run of its failed job" push master
expect_asked 1
expect_said "::notice title=Already tested::pull request #7's run https://github.com/$repo/actions/runs/101 passed on \
this tree, $tree, as its attempt 1 recorded (attempt 2 re-ran only some of its jobs)"
new_case
merged master '"2026-10-22T07:49:02Z"' pull_request success success "Tested tree" "$tree"
rerun_failed 3
expect false "the run passed on a second re-run of its failed job" push master
expect_said "as its attempt 1 recorded (attempt 3 re-ran only some of its jobs)"
new_case
merged master '"2026-10-22T07:49:02Z"' pull_request success success "Tested tree" "$other"
rerun_failed 2
expect true "the run passed on a re-run of its failed job, and its first attempt tested another tree" push master
expect_asked 5
expect_said "(#7's run 101 recorded no tree in attempt 2, and attempt 1 recorded $other)"
new_case
merged master '"2026-10-22T07:49:02Z"' pull_request success success "Tested tree" "$tree"
rerun_failed 2
answer "repos/$repo/check-runs/1013/annotations?per_page=100" "$(jq -n --arg tree "$other" \
    '[{path: ".github", start_line: 1, end_line: 1, annotation_level: "notice", title: "Tested tree", message: $tree,
      raw_details: ""}]')"
expect true "the re-run ran Tested tree again, and it recorded another tree" push master
expect_said "(#7's run 101 recorded $other)"
expect_not_asked "filter=all"
new_case
merged master '"2026-10-22T07:49:02Z"' pull_request success success "Tested tree" "$tree"
rerun_failed 2
rm -f "$answers/repos_${repo//\//_}_actions_runs_101_jobs_per_page_100_filter_all"
expect true "a re-run's earlier attempts can't be listed" push master
expect_said "(#7's run 101 recorded no tree in attempt 2, and its earlier attempts' jobs can't be listed)"
new_case
merged master '"2026-10-22T07:49:02Z"' pull_request success success "Tested tree" "$tree"
rerun_failed 2
rm -f "$answers/repos_${repo//\//_}_check-runs_1011_annotations_per_page_100"
expect true "the first attempt's record can't be read" push master
expect_said "(#7's run 101 recorded no tree in attempt 2, and job 1011's annotations (attempt 1) can't be read)"
new_case
merged master '"2026-10-22T07:49:02Z"' pull_request success success "Tested tree" "$tree"
rerun_failed 2
answer "repos/$repo/check-runs/1011/annotations?per_page=100" '[]'
expect true "no attempt of the re-run run recorded a tree" push master
expect_said "(#7's run 101 recorded no tree in any of its 2 attempts)"

# Two pull requests have the commit: the first one's run tested another tree, the second one's this tree
new_case
merged master '"2026-10-18T08:28:00Z"' pull_request success success "Tested tree" "$tree" 8 "$other" 102
pulls_8=$(cat "$answers/repos_${repo//\//_}_commits_${sha}_pulls")
merged master '"2026-10-18T08:28:00Z"' pull_request success success "Tested tree" "$other"
answer "repos/$repo/commits/$sha/pulls" "$(jq -c --argjson more "$pulls_8" '. + $more' \
    "$answers/repos_${repo//\//_}_commits_${sha}_pulls")"
expect false "the second of two pull requests passed on this tree" push master
if ! grep -q "pull request #8's run" "$work/stdout"; then
    echo "FAIL: the skip names the wrong run"
    failures=$((failures + 1))
fi

if (cd "$work/repo" && GITHUB_OUTPUT="$work/output" bash "$SCRIPT" > /dev/null 2>&1); [[ $? -eq 2 ]]; then
    echo "ok: no workflow named: usage"
else
    echo "FAIL: no workflow named: expected exit 2"
    failures=$((failures + 1))
fi
exit $((failures > 0))
