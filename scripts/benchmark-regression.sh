#!/usr/bin/env bash
# benchmark-regression.sh — ad-hoc before/after benchmark comparison
#
# This is a tool, not a gate: nothing is committed, because a comparison only
# means something on the machine that made it. Save the benchmark binary before a
# change, make the change, rebuild, then compare the two binaries.
#
# Usage:
#   ./scripts/benchmark-regression.sh --update-baseline          # save the current binary as "before"
#   ./scripts/benchmark-regression.sh --no-build --pinned 2-5    # compare "after" against it
#   ./scripts/benchmark-regression.sh --no-build --filter "Pool" # only matching benchmarks
#
# A comparison runs the saved binary and the current one in alternating rounds,
# flipping which goes first every round, so drift between runs (clock frequency,
# temperature, other load) falls on both sides alike. Each benchmark is judged by
# the median, over rounds, of its per-round after/before ratio, so one bad round
# can't decide it. Benchmarks that ask for real time (/real_time, /manual_time) are
# judged by real time, the rest by CPU time.
#
# Pin the runs to a few cores, with real-time priority where permitted: --pinned
# does this and warns about whatever it could not arrange. Pin to a range, not one
# core: the TaskManager benchmarks need worker threads (#120).
#
# The machine is rarely idle: other repositories' CI runners share this laptop. So,
# as QuickFIX's perf gate does (its #93 and #115), a comparison copes with load
# rather than waiting for none (#265):
#   - Each run waits for the one-minute load average to fall under --settle-load
#     (2.0), and starts when it does, or after --settle-max seconds (600) anyway.
#   - A pinned run also waits for each pinned CPU, and each hyperthread sibling of
#     one, to be under --settle-busy percent busy (20) over a one-second sample
#     (#406). The load average can't say which CPUs its load is on: a job pinned to
#     CPUs 1-3 held it near 2 while it saturated three of the four cores that 2-5
#     covers. The report gives each run's busiest watched CPU. A run that starts
#     with one still at --settle-busy or more, once the wait is up, shares its
#     cores with something else: the verdict is INCONCLUSIVE, and the report names
#     the run (#422).
#   - The rounds in which the baseline ran first, and the rounds in which it ran
#     second, give two estimates of each benchmark's change. When they disagree by
#     more than 25 points on more than a quarter of the benchmarks, the machine was
#     too busy to judge: the verdict is INCONCLUSIVE, and the comparison wants a
#     re-run when the machine is quieter.
#   - A benchmark faster than --min-time (200 ns) is reported but can't fail the
#     comparison: QuickFIX found run-to-run variation reaching 10% on this laptop
#     at a few nanoseconds an operation.
#
# Each run's address space is capped, at half of RAM by default (--memory-cap), so a
# benchmark that keeps more state than expected fails with exit 2 instead of pushing
# the machine into swap. Many benchmarks keep what every iteration creates until they
# end, and run more iterations the faster they get: one full pass reached 26 GB on a
# 30 GB laptop before #125.
#
# A binary reads some files from the source tree it was built from: the FIX session
# benchmarks on feature/fx-swap-support read config/fix.cfg and its dictionaries
# there (#395). By the time a comparison runs, the baseline's tree may be gone, or at
# another commit. So --update-baseline also copies the tree's config/ beside the
# baseline, as <baseline>.config, and a comparison hands that copy to the baseline in
# OP_BENCH_CONFIG_DIR, while the current build reads its own tree (#415). A benchmark
# that reports an error in the first round stops the comparison there.
#
# Exit codes: 0 = no benchmark got slower by more than the threshold, 1 = at least
# one did, 2 = usage/setup error, or a benchmark failed to run, 3 = inconclusive:
# the machine was too busy to judge, by the two estimates or by a run's busy start

set -euo pipefail

# --- Defaults ---
THRESHOLD=5
ROUNDS=6
REPETITIONS=1
DO_BUILD=true
UPDATE_BASELINE=false
BASELINE=""
BUILD_DIR=""
FILTER=""
PINNED_CORES=""
BASELINE_ARG=""
MEMORY_CAP_GIB=""
SETTLE_LOAD=2.0
SETTLE_MAX=600
SETTLE_BUSY=20
MIN_TIME_NS=200

# --- Resolve project root from script location ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

usage_error() {
    echo "ERROR: $1" >&2
    exit 2
}

# --- Argument parsing ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --threshold|--rounds|--repetitions|--baseline|--build-dir|--filter|--pinned|--memory-cap)
            [[ $# -ge 2 ]] || usage_error "$1 needs a value"
            case "$1" in
                --threshold)   THRESHOLD="$2" ;;
                --rounds)      ROUNDS="$2" ;;
                --repetitions) REPETITIONS="$2" ;;
                --baseline)    BASELINE="$2"; BASELINE_ARG=" --baseline $2" ;;
                --build-dir)   BUILD_DIR="$2" ;;
                --filter)      FILTER="$2" ;;
                --pinned)      PINNED_CORES="$2" ;;
                --memory-cap)  MEMORY_CAP_GIB="$2" ;;
            esac
            shift 2 ;;
        --settle-load|--settle-max|--settle-busy|--min-time)
            [[ $# -ge 2 ]] || usage_error "$1 needs a value"
            case "$1" in
                --settle-load) SETTLE_LOAD="$2" ;;
                --settle-max)  SETTLE_MAX="$2" ;;
                --settle-busy) SETTLE_BUSY="$2" ;;
                --min-time)    MIN_TIME_NS="$2" ;;
            esac
            shift 2 ;;
        --no-build)
            DO_BUILD=false; shift ;;
        --update-baseline)
            UPDATE_BASELINE=true; shift ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --update-baseline    Build (unless --no-build), save the benchmark binary as the"
            echo "                       baseline, and exit without running anything"
            echo "  --threshold N        Slowdown that fails the comparison, in percent (default: 5)"
            echo "  --rounds N           Alternating rounds of baseline and current runs (default: 6)"
            echo "  --repetitions N      Repetitions per binary per round (default: 1)"
            echo "  --no-build           Skip the clean build (use the existing build)"
            echo "  --baseline FILE      Baseline binary (default: benchmark_baseline.bin in the repo root)"
            echo "  --build-dir DIR      Override build directory path"
            echo "  --filter REGEX       Run only matching benchmarks"
            echo "  --pinned CORES       Pin the runs to CORES (taskset list; use a range such as 2-5)"
            echo "                       and request real-time priority where permitted"
            echo "  --memory-cap GIB     Cap each benchmark run's address space at GIB GiB (default:"
            echo "                       half of RAM; 0 for no cap, e.g. for a sanitizer build)"
            echo "  --settle-load N      Start each run once the one-minute load average is under N"
            echo "                       (default: 2.0)"
            echo "  --settle-max SECONDS Wait at most SECONDS for that, then run anyway (default: 600;"
            echo "                       0 never waits)"
            echo "  --settle-busy PCT    With --pinned, also wait until each pinned CPU and each"
            echo "                       hyperthread sibling of one is under PCT percent busy (default: 20);"
            echo "                       a run that starts with one at PCT or more makes it INCONCLUSIVE"
            echo "  --min-time NS        Report, but never fail on, a benchmark faster than NS"
            echo "                       nanoseconds (default: 200)"
            echo "  -h, --help           Show this help"
            exit 0
            ;;
        *)
            usage_error "Unknown option: $1" ;;
    esac
done

[[ "$THRESHOLD" =~ ^[0-9]+([.][0-9]+)?$ ]] || usage_error "--threshold needs a number, got '$THRESHOLD'"
# 10# keeps a leading zero from making bash read the number as octal.
if ! [[ "$ROUNDS" =~ ^[0-9]+$ ]] || (( 10#$ROUNDS < 2 )); then
    usage_error "--rounds needs a whole number of at least 2, got '$ROUNDS'"
fi
if ! [[ "$REPETITIONS" =~ ^[0-9]+$ ]] || (( 10#$REPETITIONS < 1 )); then
    usage_error "--repetitions needs a whole number of at least 1, got '$REPETITIONS'"
fi
ROUNDS=$(( 10#$ROUNDS ))
REPETITIONS=$(( 10#$REPETITIONS ))
[[ "$SETTLE_LOAD" =~ ^[0-9]+([.][0-9]+)?$ ]] || usage_error "--settle-load needs a number, got '$SETTLE_LOAD'"
[[ "$SETTLE_MAX" =~ ^[0-9]+$ ]] || usage_error "--settle-max needs a whole number of seconds, got '$SETTLE_MAX'"
SETTLE_MAX=$(( 10#$SETTLE_MAX ))
if ! [[ "$SETTLE_BUSY" =~ ^[0-9]+$ ]] || (( 10#$SETTLE_BUSY < 1 || 10#$SETTLE_BUSY > 100 )); then
    usage_error "--settle-busy needs a whole number of percent from 1 to 100, got '$SETTLE_BUSY'"
fi
SETTLE_BUSY=$(( 10#$SETTLE_BUSY ))
# The CPUs a pinned run shares with anything else (#406): the pinned ones and their hyperthread siblings
WATCHED=""
if [[ -n "$PINNED_CORES" ]]; then
    WATCHED=$("$SCRIPT_DIR/cpu-busy.sh" watched "$PINNED_CORES") ||
        usage_error "--pinned needs a list of CPUs, such as 2-5, got '$PINNED_CORES'"
    # Here, before anything is built, rather than at the first run's settle
    "$SCRIPT_DIR/cpu-busy.sh" busiest "$WATCHED" /proc/stat /proc/stat > /dev/null ||
        usage_error "--pinned names a CPU that isn't online here: '$PINNED_CORES' watches CPUs $WATCHED"
fi
[[ "$MIN_TIME_NS" =~ ^[0-9]+([.][0-9]+)?$ ]] || usage_error "--min-time needs a number of nanoseconds, got '$MIN_TIME_NS'"
if [[ -n "$MEMORY_CAP_GIB" ]]; then
    [[ "$MEMORY_CAP_GIB" =~ ^[0-9]+$ ]] || usage_error "--memory-cap needs a whole number of GiB (0 for no cap), got '$MEMORY_CAP_GIB'"
    MEMORY_CAP_GIB=$(( 10#$MEMORY_CAP_GIB ))
else
    # Half of RAM: room for any benchmark today (a full pass peaks near 2 GB) and for whatever else
    # shares the machine. If RAM can't be read, don't cap.
    MEM_TOTAL_KB=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || true)
    MEMORY_CAP_GIB=$(( ${MEM_TOTAL_KB:-0} / 2 / 1024 / 1024 ))
fi

BASELINE="${BASELINE:-$PROJECT_ROOT/benchmark_baseline.bin}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
# Made absolute once (#180): the binaries run from a scratch directory, where a relative path given here named nothing,
# and --update-baseline and a comparison have to agree on where the baseline is
BASELINE="$(realpath -m -- "$BASELINE")"
BUILD_DIR="$(realpath -m -- "$BUILD_DIR")"
if [[ -n "$BASELINE_ARG" ]]; then
    BASELINE_ARG=" --baseline $BASELINE"
fi
BENCH_EXE="$BUILD_DIR/orderProcessorBench"

# The benchmarks run in a scratch directory: they write exchange.log in their
# working directory, gigabytes per run with note logging on (#121).
WORK=$(mktemp -d /tmp/benchmark_compare_XXXXXX)

# Exit status 1 belongs to the verdict alone. Anything else that stops the script
# (a failed build, copy or run, set -e, an interrupt) is a setup error and exits 2,
# so it can't read as a slowdown.
VERDICT=""
# shellcheck disable=SC2329 # trap runs it, on EXIT below
on_exit() {
    local rc=$?
    rm -rf -- "${WORK:?}"
    if [[ -z "$VERDICT" && $rc -ne 0 && $rc -ne 2 ]]; then
        exit 2
    fi
    exit "$rc"
}
trap on_exit EXIT

describe_commit() {
    local sha
    sha=$(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null) || { echo "(not a git checkout)"; return; }
    if git -C "$PROJECT_ROOT" diff --quiet HEAD -- 2>/dev/null; then
        echo "$sha"
    else
        echo "$sha with uncommitted changes"
    fi
}

describe_compiler() {
    # The compiler the benchmark was built with, not whatever $CXX is now.
    local compiler
    compiler=$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || true)
    "${compiler:-${CXX:-c++}}" --version 2>/dev/null | head -1
}

# The source tree the build was configured from, whose config/ the binary reads (#415)
build_source_dir() {
    local dir
    dir=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || true)
    echo "${dir:-$PROJECT_ROOT}"
}

# --- Step 1: Build ---
if $DO_BUILD; then
    echo "=== Clean Release Build ==="
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF -DBUILD_APP=OFF
    cmake --build "$BUILD_DIR" -j"$(nproc)"
    echo ""
fi

if [[ ! -x "$BENCH_EXE" ]]; then
    echo "ERROR: Benchmark executable not found: $BENCH_EXE" >&2
    echo "Run without --no-build or build first." >&2
    exit 2
fi

# --- Step 2: Save the baseline, if asked ---
if $UPDATE_BASELINE; then
    # The binary links only system libraries, so a copy keeps working after rebuilds. The files it reads from its source
    # tree don't come with it: a copy of the tree's config/ goes beside it (#415). Without a config/ (master), any copy
    # an earlier save left at this path goes, so a comparison never hands the binary another tree's.
    SOURCE_DIR=$(build_source_dir)
    cp "$BENCH_EXE" "$BASELINE.tmp"
    rm -rf -- "${BASELINE:?}.config.tmp"
    if [[ -d "$SOURCE_DIR/config" ]]; then
        cp -R -- "$SOURCE_DIR/config" "$BASELINE.config.tmp"
    fi
    mv "$BASELINE.tmp" "$BASELINE"
    rm -rf -- "${BASELINE:?}.config"
    if [[ -d "$BASELINE.config.tmp" ]]; then
        mv -- "$BASELINE.config.tmp" "$BASELINE.config"
    fi
    {
        echo "commit:    $(describe_commit)"
        echo "compiler:  $(describe_compiler)"
        echo "saved:     $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(uname -n)"
        if [[ -d "$BASELINE.config" ]]; then
            echo "config:    $BASELINE.config, a copy of $SOURCE_DIR/config"
        fi
    } > "$BASELINE.info"
    echo "=== Baseline Saved ==="
    echo "Saved $BENCH_EXE as $BASELINE"
    sed 's/^/  /' "$BASELINE.info"
    echo "It belongs to this machine and toolchain, and is deliberately not committed."
    echo "Make your change and rebuild, then compare:"
    # shellcheck disable=SC2016 # the quotes are text in the command printed; $FILTER does expand inside the "..."
    echo "  $0 --no-build --pinned ${PINNED_CORES:-2-5}${BASELINE_ARG}${FILTER:+ --filter '$FILTER'}"
    exit 0
fi

# --- Step 3: Check the baseline ---
if [[ ! -e "$BASELINE" ]]; then
    echo "ERROR: No baseline to compare against: $BASELINE" >&2
    echo "" >&2
    echo "A baseline is the benchmark binary from before your change, saved on this machine." >&2
    echo "Save one first, with the code from before the change built:" >&2
    echo "  $0 --update-baseline" >&2
    echo "then make your change, rebuild, and re-run this command." >&2
    exit 2
fi
if [[ ! -f "$BASELINE" ]]; then
    echo "ERROR: Baseline is not a file: $BASELINE" >&2
    exit 2
fi
if [[ "$(head -c 1 "$BASELINE")" == "{" ]]; then
    echo "ERROR: $BASELINE holds benchmark results from an older version of this script." >&2
    echo "A baseline is now the benchmark binary itself. Build the code from before your" >&2
    echo "change and save it with: $0 --update-baseline" >&2
    exit 2
fi
if [[ ! -x "$BASELINE" ]]; then
    echo "ERROR: Baseline is not an executable: $BASELINE" >&2
    exit 2
fi

FILTER_ARGS=()
if [[ -n "$FILTER" ]]; then
    FILTER_ARGS=("--benchmark_filter=$FILTER")
fi
# The number of the binary's benchmarks the filter matches. A binary that lists nothing says why on standard error: that
# was discarded, so a binary that couldn't run read as a filter that matched nothing (#180).
count_benchmarks() {
    local listed
    listed=$( cd "$WORK" && "$1" --benchmark_list_tests=true "${FILTER_ARGS[@]}" 2>"$WORK/list-errors" ) || true
    if [[ -z "$listed" && -s "$WORK/list-errors" ]]; then
        echo "  $1 listed nothing:" >&2
        sed 's/^/    /' "$WORK/list-errors" >&2
    fi
    grep -c . <<< "$listed" || true
}
AFTER_COUNT=$(count_benchmarks "$BENCH_EXE")
BEFORE_COUNT=$(count_benchmarks "$BASELINE")
if (( AFTER_COUNT == 0 && BEFORE_COUNT == 0 )); then
    # shellcheck disable=SC2016 # the quotes are text in the message; $FILTER does expand inside the "..."
    echo "ERROR: No benchmark matches${FILTER:+ --filter '$FILTER'} in either binary" >&2
    exit 2
fi

# --- Step 4: Describe what is being measured ---
echo "=== Comparing Benchmarks: $ROUNDS alternating rounds, $REPETITIONS repetition(s) per run ==="
echo "--- machine ---"
echo "  host:      $(uname -n)  kernel $(uname -r)"
echo "  cpu:       $(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')  ($(nproc) threads)"
echo "  governor:  $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"

GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)
if [[ "$GOVERNOR" != "performance" && "$GOVERNOR" != "unknown" ]]; then
    echo "  WARNING: CPU governor is '$GOVERNOR'; frequency scaling adds run-to-run noise." >&2
fi

# Build the launcher: taskset pins the runs to the given cores, chrt asks for
# real-time priority. chrt needs privileges (ulimit -r), so a failure downgrades
# to taskset alone rather than aborting the run.
LAUNCH=()
if [[ -n "$PINNED_CORES" ]]; then
    if ! command -v taskset >/dev/null 2>&1; then
        echo "ERROR: --pinned needs taskset (util-linux)" >&2
        exit 2
    fi
    LAUNCH=(taskset -c "$PINNED_CORES")
    if command -v chrt >/dev/null 2>&1 && chrt -f 50 true >/dev/null 2>&1; then
        LAUNCH=(chrt -f 50 "${LAUNCH[@]}")
        echo "  pinned:    cores $PINNED_CORES, SCHED_FIFO 50"
    else
        echo "  pinned:    cores $PINNED_CORES (no real-time priority: needs privileges, see 'ulimit -r')"
    fi
    if [[ "$PINNED_CORES" =~ ^[0-9]+$ ]]; then
        echo "  WARNING: pinned to one core; the TaskManager benchmarks can't run there (#120). Pin to a range." >&2
    fi
else
    echo "  pinned:    no (pass --pinned CORES for comparable numbers)"
fi
if (( MEMORY_CAP_GIB > 0 )); then
    echo "  memory:    each run capped at $MEMORY_CAP_GIB GiB of address space (--memory-cap; 0 for none)"
else
    echo "  memory:    no cap (--memory-cap 0)"
fi
now=$(cut -d' ' -f1 /proc/loadavg)
if (( SETTLE_MAX > 0 )); then
    echo "  settle:    each run waits up to $SETTLE_MAX s for a one-minute load under $SETTLE_LOAD (now $now)"
else
    echo "  settle:    none (--settle-max 0); one-minute load now $now"
fi
if [[ -n "$WATCHED" ]]; then
    echo "  watched:   CPUs $WATCHED, the pinned ones and their hyperthread siblings; each under $SETTLE_BUSY% busy before a run (#406)"
fi
echo "--- before (baseline) ---"
echo "  binary:    $BASELINE"
if [[ -f "$BASELINE.info" ]]; then
    sed 's/^/  /' "$BASELINE.info"
fi
if [[ ! -d "$BASELINE.config" && -d "$(build_source_dir)/config" ]]; then
    echo "  config:    none saved with it (#415): it reads config/ from the tree it was built in, if that is still there"
fi
echo "  matches:   $BEFORE_COUNT benchmark(s)"
echo "--- after (current build) ---"
echo "  binary:    $BENCH_EXE"
echo "  commit:    $(describe_commit)"
echo "  compiler:  $(describe_compiler)"
echo "  matches:   $AFTER_COUNT benchmark(s)"
if cmp -s "$BASELINE" "$BENCH_EXE"; then
    echo "  note:      the two binaries are identical, so this measures noise only"
fi
echo ""

# --- Step 5: Run the rounds ---
BENCH_ARGS=(--benchmark_out_format=json "--benchmark_repetitions=$REPETITIONS" "${FILTER_ARGS[@]}")

# Runs in the subshell that starts a benchmark, so the cap applies to that run alone.
limit_memory() {
    if (( MEMORY_CAP_GIB > 0 )); then
        ulimit -v $(( MEMORY_CAP_GIB * 1024 * 1024 ))
    fi
}

# The busiest watched CPU over a one-second sample, as "<cpu> <percent busy>" (#406); "- 0" when the runs aren't pinned
busiest_watched() {
    if [[ -z "$WATCHED" ]]; then
        echo "- 0"
        return
    fi
    cat /proc/stat > "$WORK/stat.before"
    sleep 1
    cat /proc/stat > "$WORK/stat.after"
    "$SCRIPT_DIR/cpu-busy.sh" busiest "$WATCHED" "$WORK/stat.before" "$WORK/stat.after"
}

# As QuickFIX's ptgate.sh does (#265): wait until the one-minute load average is under SETTLE_LOAD, and every watched
# CPU under SETTLE_BUSY percent busy (#406), for SETTLE_MAX seconds at most, then let the run start regardless. Sets
# SETTLE_AT, the load it started at, SETTLE_BUSIEST, its busiest watched CPU then and how busy, and SETTLE_WAITED, the
# seconds it waited, a pinned run's samples included.
settle() {
    local began step
    began=$(date +%s)
    while :; do
        SETTLE_BUSIEST=""
        if awk -v max="$SETTLE_LOAD" '{ exit !($1 + 0 < max + 0) }' /proc/loadavg; then
            SETTLE_BUSIEST=$(busiest_watched)
            if (( ${SETTLE_BUSIEST##* } < SETTLE_BUSY )); then
                break
            fi
        fi
        SETTLE_WAITED=$(( $(date +%s) - began ))
        (( SETTLE_WAITED < SETTLE_MAX )) || break
        step=$(( SETTLE_MAX - SETTLE_WAITED < 10 ? SETTLE_MAX - SETTLE_WAITED : 10 ))
        sleep "$step"
    done
    [[ -n "$SETTLE_BUSIEST" ]] || SETTLE_BUSIEST=$(busiest_watched)
    SETTLE_WAITED=$(( $(date +%s) - began ))
    SETTLE_AT=$(cut -d' ' -f1 /proc/loadavg)
}

# Where each side reads its source tree's config/ (#415): the baseline the copy saved with it, if there is one, and the
# current build its own tree. Whatever OP_BENCH_CONFIG_DIR the caller has set reaches neither.
BEFORE_ENV=(env -u OP_BENCH_CONFIG_DIR)
if [[ -d "$BASELINE.config" ]]; then
    BEFORE_ENV=(env "OP_BENCH_CONFIG_DIR=$BASELINE.config")
fi
AFTER_ENV=(env -u OP_BENCH_CONFIG_DIR)

# Leaves the run's starting load, its wait and its own duration in r<round>-<side>.load, for the report.
run_side() {  # round, side, binary
    local out="$WORK/r$1-$2.json" log="$WORK/r$1-$2.log" began
    local -a side_env=("${AFTER_ENV[@]}")
    if [[ before == "$2" ]]; then
        side_env=("${BEFORE_ENV[@]}")
    fi
    settle
    began=$(date +%s)
    if ! ( cd "$WORK" && limit_memory && "${side_env[@]}" "${LAUNCH[@]}" "$3" "${BENCH_ARGS[@]}" "--benchmark_out=$out" ) \
        > "$log" 2>&1; then
        echo "ERROR: the $2 binary failed in round $1. The last lines of its output:" >&2
        tail -n 20 "$log" >&2
        if (( MEMORY_CAP_GIB > 0 )) && grep -q -E 'bad_alloc|Cannot allocate memory|out of memory' "$log"; then
            echo "It ran out of memory under the $MEMORY_CAP_GIB GiB cap: a benchmark kept more state than that." >&2
            echo "Narrow --filter, or raise --memory-cap if the machine has room." >&2
        fi
        exit 2
    fi
    echo "$SETTLE_AT $SETTLE_WAITED $(( $(date +%s) - began )) $SETTLE_BUSIEST" > "$WORK/r$1-$2.load"
}

# A benchmark that can't run (a file it reads missing, a setup that fails) reports an error in every round. After the
# first round this prints each one that did, and fails, so the comparison stops there rather than after every round
# (#415).
first_round_errors() {
    python3 - "$WORK/r1-before.json" "$WORK/r1-after.json" <<'PYEOF'
import json, sys

errors = {}
for side, path in zip(('before', 'after'), sys.argv[1:]):
    try:
        with open(path) as f:
            report = json.load(f)
    except (OSError, ValueError) as e:
        errors[f'(the {side} run)'] = f'its report could not be read: {e}'
        continue
    for b in report.get('benchmarks', []):
        if b.get('error_occurred'):
            errors.setdefault(b.get('run_name', b['name']), f"{side}: {b.get('error_message') or 'error'}")
if errors:
    print(f"\n*** ERROR: {len(errors)} benchmark(s) did not produce a measurement in round 1, "
          "so the comparison stops there ***")
    for name in sorted(errors):
        print(f"  ! {name}: {errors[name]}")
    sys.exit(1)
PYEOF
}

for (( r = 1; r <= ROUNDS; r++ )); do
    if (( r % 2 )); then
        order=(before after)
    else
        order=(after before)
    fi
    took=0 starts=""
    for side in "${order[@]}"; do
        if [[ "$side" == before ]]; then
            run_side "$r" before "$BASELINE"
        else
            run_side "$r" after "$BENCH_EXE"
        fi
        read -r load waited ran busiest busy < "$WORK/r$r-$side.load"
        took=$(( took + ran ))
        starts+="${starts:+, }$side at load $load${WATCHED:+ (CPU $busiest $busy% busy)} after ${waited} s"
    done
    if (( r == 1 )); then
        eta=$(( (took * (ROUNDS - 1) + 59) / 60 ))
        echo "  round 1/$ROUNDS: $starts; ${took} s of runs (about $eta min more, and the waits)"
        if ! first_round_errors; then
            exit 2
        fi
    else
        echo "  round $r/$ROUNDS: $starts; ${took} s of runs"
    fi
done
echo ""

# --- Step 6: Judge and report ---
set +e
python3 - "$THRESHOLD" "$ROUNDS" "$WORK" "$MIN_TIME_NS" "$SETTLE_LOAD" "$SETTLE_MAX" "$SETTLE_BUSY" "$WATCHED" <<'PYEOF'
import json, os, statistics, sys

threshold_pct = float(sys.argv[1])
rounds = int(sys.argv[2])
work = sys.argv[3]
min_time_ns = float(sys.argv[4])
settle_load = float(sys.argv[5])
settle_max = int(sys.argv[6])
settle_busy = int(sys.argv[7])
watched = sys.argv[8]
limit = threshold_pct / 100.0
# As QuickFIX's ptcompare.py (#265): the rounds in which the baseline ran first, and the rest, give two estimates of
# a change. A benchmark is noisy when they differ by more than NOISE, and more than NOISY_SHARE of the benchmarks
# noisy means the machine was too busy for either verdict.
NOISE = 0.25
NOISY_SHARE = 0.25
NS_PER_UNIT = {'ns': 1.0, 'us': 1e3, 'ms': 1e6, 's': 1e9}


def load(path):
    """Return {name: (timer, unit, [values])} and {name: error message} for one run."""
    with open(path) as f:
        report = json.load(f)
    runs, errors = {}, {}
    for b in report.get('benchmarks', []):
        if b.get('run_type') == 'aggregate':
            continue
        name = b.get('run_name', b['name'])
        if b.get('error_occurred'):
            errors[name] = b.get('error_message') or 'error'
            continue
        timer = 'real_time' if name.endswith(('/real_time', '/manual_time')) else 'cpu_time'
        runs.setdefault(name, (timer, b.get('time_unit', 'ns'), []))[2].append(float(b[timer]))
    return runs, errors


def fmt(value, unit):
    return f"{value:,.0f} {unit}" if value >= 100 else f"{value:.2f} {unit}"


def main():
    before, after, errors = [], [], {}
    for r in range(1, rounds + 1):
        for side, store in (('before', before), ('after', after)):
            runs, errs = load(os.path.join(work, f'r{r}-{side}.json'))
            store.append(runs)
            for name, message in errs.items():
                errors.setdefault(name, f'{side}, round {r}: {message}')

    def present_in_every_round(side):
        return set.intersection(*(set(runs) for runs in side))

    b_names, a_names = present_in_every_round(before), present_in_every_round(after)
    common = sorted((b_names & a_names) - set(errors))
    added = sorted(a_names - b_names - set(errors))
    removed = sorted(b_names - a_names - set(errors))

    rows = []
    for name in common:
        timer, unit, _ = after[0][name]
        b_meds = [statistics.median(before[r][name][2]) for r in range(rounds)]
        a_meds = [statistics.median(after[r][name][2]) for r in range(rounds)]
        if min(b_meds) <= 0:
            errors[name] = 'the baseline measured zero time'
            continue
        ratios = [a / b for a, b in zip(a_meds, b_meds)]
        # Odd rounds ran the baseline first, even rounds second
        halves = (statistics.median(ratios[0::2]) - 1.0, statistics.median(ratios[1::2]) - 1.0)
        rows.append({
            'name': name, 'timer': 'real' if timer == 'real_time' else 'cpu', 'unit': unit,
            'before': statistics.median(b_meds), 'after': statistics.median(a_meds),
            'change': statistics.median(ratios) - 1.0,
            'slower': sum(1 for x in ratios if x > 1.0),
            'lo': min(ratios) - 1.0, 'hi': max(ratios) - 1.0,
            'halves': halves, 'noisy': abs(halves[0] - halves[1]) > NOISE,
            'tiny': statistics.median(b_meds) * NS_PER_UNIT.get(unit, 1.0) < min_time_ns,
        })

    past_limit = sorted((r for r in rows if r['change'] > limit), key=lambda r: -r['change'])
    regressions = [r for r in past_limit if not r['tiny']]
    tiny_slower = [r for r in past_limit if r['tiny']]
    improvements = sorted((r for r in rows if r['change'] < -limit), key=lambda r: r['change'])
    noisy = sorted((r for r in rows if r['noisy']), key=lambda r: -abs(r['halves'][0] - r['halves'][1]))

    print("=" * 100)
    print(f"  BENCHMARK COMPARISON  ({len(rows)} benchmarks, {rounds} rounds, threshold {threshold_pct:.1f}%)")
    print("  change = median over rounds of after/before; slower = rounds in which after was slower;")
    print("  range = lowest..highest per-round change; timer = real for /real_time and /manual_time, else cpu")
    print("  halves = the change over the rounds that ran the baseline first / second; noisy when they differ")
    print(f"  by more than {NOISE * 100:.0f} points. A benchmark under {min_time_ns:g} ns is reported but can't fail")
    print("=" * 100)

    width = min(60, max([len(r['name']) for r in rows] + [9]))

    def table(title, entries):
        print(f"\n{title} ({len(entries)}):")
        print(f"  {'Benchmark':<{width}s} {'timer':>5s} {'before':>14s} {'after':>14s} {'change':>8s} {'slower':>7s}"
              f"  {'range':<15s} halves")
        for r in entries:
            spread = f"{r['lo'] * 100:+.1f}..{r['hi'] * 100:+.1f}%"
            print(f"  {r['name']:<{width}s} {r['timer']:>5s} {fmt(r['before'], r['unit']):>14s} {fmt(r['after'], r['unit']):>14s}"
                  f" {r['change'] * 100:>+7.1f}% {r['slower']:>3d}/{rounds:<3d}  {spread:<15s}"
                  f" {r['halves'][0] * 100:+.1f}/{r['halves'][1] * 100:+.1f}%")

    if improvements:
        table("Faster by more than the threshold", improvements)
    if regressions:
        table("SLOWER by more than the threshold", regressions)
    if tiny_slower:
        table(f"Slower by more than the threshold, but under {min_time_ns:g} ns, so not failing", tiny_slower)
    if noisy:
        table(f"Noisy: the two halves differ by more than {NOISE * 100:.0f} points", noisy)
    if rows:
        overall = statistics.median(r['change'] for r in rows) * 100
        print(f"\nMedian change across all {len(rows)} compared benchmarks: {overall:+.1f}%")
    if added:
        print(f"\nWARNING: {len(added)} benchmark(s) only in the current binary (not compared):")
        for n in added:
            print(f"  + {n}")
    if removed:
        print(f"\nWARNING: {len(removed)} benchmark(s) only in the baseline (not compared):")
        for n in removed:
            print(f"  - {n}")

    if errors:
        print(f"\n*** ERROR: {len(errors)} benchmark(s) did not produce a measurement, so this comparison is incomplete ***")
        for name in sorted(errors):
            print(f"  ! {name}: {errors[name]}")
        return 2
    if not rows:
        print("\n*** ERROR: no benchmark ran in both binaries ***")
        return 2
    loads = []
    for r in range(1, rounds + 1):
        for side in ('before', 'after'):
            with open(os.path.join(work, f'r{r}-{side}.load')) as f:
                started_at, waited, _, cpu, busy = f.read().split()
                loads.append((float(started_at), int(waited), cpu, int(busy), r, side))
    full = sum(1 for x in loads if settle_max and x[1] >= settle_max)
    lo, hi = min(x[0] for x in loads), max(x[0] for x in loads)
    line = f"\nThe {len(loads)} runs started at one-minute loads {lo:.2f}..{hi:.2f}"
    if full:
        line += f"; {full} of them after the full {settle_max} s wait, with the load still at {settle_load:g} or more"
        if watched:
            line += f", or a watched CPU {settle_busy}% busy or more"
    print(line)
    busy_starts = []
    if watched:
        busy = [x[3] for x in loads]
        print(f"Their busiest watched CPU (of {watched}) was {min(busy)}..{max(busy)}% busy as they started")
        busy_starts = [x for x in loads if x[3] >= settle_busy]
        if busy_starts:
            print(f"\nStarted with a watched CPU {settle_busy}% busy or more ({len(busy_starts)}):")
            for _, _, cpu, pct, r, side in busy_starts:
                print(f"  round {r}, {side}: CPU {cpu} {pct}% busy")
    # Too busy to judge, ahead of FAIL and PASS: a run that started with a watched CPU busy, once its wait was up,
    # shared its cores with something else (#422, as ruled), and two halves that disagree say the same of the rounds
    inconclusive = False
    if busy_starts:
        print(f"\n*** INCONCLUSIVE: {len(busy_starts)} of the {len(loads)} runs started with a watched CPU "
              f"{settle_busy}% busy or more: something else ran on their cores. Re-run when it is quieter ***")
        inconclusive = True
    if len(noisy) > NOISY_SHARE * len(rows):
        print(f"\n*** INCONCLUSIVE: the two halves differ by more than {NOISE * 100:.0f} points on {len(noisy)} of "
              f"{len(rows)} benchmarks: the machine was too busy to judge. Re-run when it is quieter ***")
        inconclusive = True
    if inconclusive:
        return 3
    if regressions:
        print(f"\n*** FAIL: {len(regressions)} benchmark(s) slower by more than {threshold_pct:.1f}% ***")
        return 1
    print(f"\n*** PASS: no benchmark slower by more than {threshold_pct:.1f}% (median of {rounds} rounds) ***")
    return 0


try:
    sys.exit(main())
except SystemExit:
    raise
except Exception as e:  # a crash here must not read as a regression (exit 1)
    print(f"ERROR: could not analyse the runs: {type(e).__name__}: {e}", file=sys.stderr)
    sys.exit(2)
PYEOF
RESULT=$?
set -e

if (( RESULT == 2 )) && [[ "$PINNED_CORES" =~ ^[0-9]+$ ]]; then
    echo "The runs were pinned to a single core, where the TaskManager benchmarks can't run (#120)." >&2
fi
VERDICT=$RESULT
exit "$RESULT"
