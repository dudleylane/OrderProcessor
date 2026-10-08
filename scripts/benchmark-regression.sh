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
# Trustworthy numbers need an otherwise idle machine and the run pinned to a few
# isolated cores, with real-time priority where permitted: --pinned does this and
# warns about whatever it could not arrange. Pin to a range, not one core: the
# TaskManager benchmarks need worker threads (#120).
#
# Each run's address space is capped, at half of RAM by default (--memory-cap), so a
# benchmark that keeps more state than expected fails with exit 2 instead of pushing
# the machine into swap. Many benchmarks keep what every iteration creates until they
# end, and run more iterations the faster they get: one full pass reached 26 GB on a
# 30 GB laptop before #125.
#
# Exit codes: 0 = no benchmark got slower by more than the threshold, 1 = at least
# one did, 2 = usage/setup error, or a benchmark failed to run

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
            echo "  -h, --help           Show this help"
            exit 0
            ;;
        *)
            usage_error "Unknown option: $1" ;;
    esac
done

[[ "$THRESHOLD" =~ ^[0-9]+([.][0-9]+)?$ ]] || usage_error "--threshold needs a number, got '$THRESHOLD'"
# 10# keeps a leading zero from making bash read the number as octal.
[[ "$ROUNDS" =~ ^[0-9]+$ ]] && (( 10#$ROUNDS >= 2 )) || usage_error "--rounds needs a whole number of at least 2, got '$ROUNDS'"
[[ "$REPETITIONS" =~ ^[0-9]+$ ]] && (( 10#$REPETITIONS >= 1 )) || usage_error "--repetitions needs a whole number of at least 1, got '$REPETITIONS'"
ROUNDS=$(( 10#$ROUNDS ))
REPETITIONS=$(( 10#$REPETITIONS ))
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
BENCH_EXE="$BUILD_DIR/orderProcessorBench"

# The benchmarks run in a scratch directory: they write exchange.log in their
# working directory, gigabytes per run with note logging on (#121).
WORK=$(mktemp -d /tmp/benchmark_compare_XXXXXX)

# Exit status 1 belongs to the verdict alone. Anything else that stops the script
# (a failed build, copy or run, set -e, an interrupt) is a setup error and exits 2,
# so it can't read as a slowdown.
VERDICT=""
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
    # The binary links only system libraries, so a copy keeps working after rebuilds.
    cp "$BENCH_EXE" "$BASELINE.tmp"
    mv "$BASELINE.tmp" "$BASELINE"
    {
        echo "commit:    $(describe_commit)"
        echo "compiler:  $(describe_compiler)"
        echo "saved:     $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(uname -n)"
    } > "$BASELINE.info"
    echo "=== Baseline Saved ==="
    echo "Saved $BENCH_EXE as $BASELINE"
    sed 's/^/  /' "$BASELINE.info"
    echo "It belongs to this machine and toolchain, and is deliberately not committed."
    echo "Make your change and rebuild, then compare:"
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
count_benchmarks() {
    ( cd "$WORK" && "$1" --benchmark_list_tests=true "${FILTER_ARGS[@]}" 2>/dev/null ) | grep -c . || true
}
AFTER_COUNT=$(count_benchmarks "$BENCH_EXE")
BEFORE_COUNT=$(count_benchmarks "$BASELINE")
if (( AFTER_COUNT == 0 && BEFORE_COUNT == 0 )); then
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
echo "--- before (baseline) ---"
echo "  binary:    $BASELINE"
if [[ -f "$BASELINE.info" ]]; then
    sed 's/^/  /' "$BASELINE.info"
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

run_side() {  # round, side, binary
    local out="$WORK/r$1-$2.json" log="$WORK/r$1-$2.log"
    if ! ( cd "$WORK" && limit_memory && "${LAUNCH[@]}" "$3" "${BENCH_ARGS[@]}" "--benchmark_out=$out" ) > "$log" 2>&1; then
        echo "ERROR: the $2 binary failed in round $1. The last lines of its output:" >&2
        tail -n 20 "$log" >&2
        if (( MEMORY_CAP_GIB > 0 )) && grep -q -E 'bad_alloc|Cannot allocate memory|out of memory' "$log"; then
            echo "It ran out of memory under the $MEMORY_CAP_GIB GiB cap: a benchmark kept more state than that." >&2
            echo "Narrow --filter, or raise --memory-cap if the machine has room." >&2
        fi
        exit 2
    fi
}

for (( r = 1; r <= ROUNDS; r++ )); do
    if (( r % 2 )); then
        order=(before after)
    else
        order=(after before)
    fi
    started=$(date +%s)
    for side in "${order[@]}"; do
        if [[ "$side" == before ]]; then
            run_side "$r" before "$BASELINE"
        else
            run_side "$r" after "$BENCH_EXE"
        fi
    done
    took=$(( $(date +%s) - started ))
    if (( r == 1 )); then
        echo "  round 1/$ROUNDS: ${order[0]} first, ${took} s (about $(( (took * (ROUNDS - 1) + 59) / 60 )) min to go)"
    else
        echo "  round $r/$ROUNDS: ${order[0]} first, ${took} s"
    fi
done
echo ""

# --- Step 6: Judge and report ---
set +e
python3 - "$THRESHOLD" "$ROUNDS" "$WORK" <<'PYEOF'
import json, os, statistics, sys

threshold_pct = float(sys.argv[1])
rounds = int(sys.argv[2])
work = sys.argv[3]
limit = threshold_pct / 100.0


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
        rows.append({
            'name': name, 'timer': 'real' if timer == 'real_time' else 'cpu', 'unit': unit,
            'before': statistics.median(b_meds), 'after': statistics.median(a_meds),
            'change': statistics.median(ratios) - 1.0,
            'slower': sum(1 for x in ratios if x > 1.0),
            'lo': min(ratios) - 1.0, 'hi': max(ratios) - 1.0,
        })

    regressions = sorted((r for r in rows if r['change'] > limit), key=lambda r: -r['change'])
    improvements = sorted((r for r in rows if r['change'] < -limit), key=lambda r: r['change'])

    print("=" * 100)
    print(f"  BENCHMARK COMPARISON  ({len(rows)} benchmarks, {rounds} rounds, threshold {threshold_pct:.1f}%)")
    print("  change = median over rounds of after/before; slower = rounds in which after was slower;")
    print("  range = lowest..highest per-round change; timer = real for /real_time and /manual_time, else cpu")
    print("=" * 100)

    width = min(60, max([len(r['name']) for r in rows] + [9]))

    def table(title, entries):
        print(f"\n{title} ({len(entries)}):")
        print(f"  {'Benchmark':<{width}s} {'timer':>5s} {'before':>14s} {'after':>14s} {'change':>8s} {'slower':>7s}  range")
        for r in entries:
            print(f"  {r['name']:<{width}s} {r['timer']:>5s} {fmt(r['before'], r['unit']):>14s} {fmt(r['after'], r['unit']):>14s}"
                  f" {r['change'] * 100:>+7.1f}% {r['slower']:>3d}/{rounds:<3d}  {r['lo'] * 100:+.1f}..{r['hi'] * 100:+.1f}%")

    if improvements:
        table("Faster by more than the threshold", improvements)
    if regressions:
        table("SLOWER by more than the threshold", regressions)
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
