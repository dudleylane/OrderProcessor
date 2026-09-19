#!/usr/bin/env bash
# benchmark-regression.sh — ad-hoc before/after benchmark comparison
#
# This is a tool, not a gate: no baseline is committed, because a baseline is only
# meaningful on the machine that produced it. Create one on the machine you are
# measuring, change the code, then compare against it.
#
# Usage:
#   ./scripts/benchmark-regression.sh --update-baseline          # record "before" on this machine
#   ./scripts/benchmark-regression.sh --no-build                 # compare "after" against it
#   ./scripts/benchmark-regression.sh --pinned 3 --no-build      # pin to core 3 (see below)
#   ./scripts/benchmark-regression.sh --filter "Pool"            # only matching benchmarks
#
# Trustworthy numbers need an otherwise idle machine, the benchmark pinned to an
# isolated core and, where permitted, real-time priority: --pinned does this and
# warns about whatever it could not arrange.
#
# Exit codes: 0 = no change beyond the threshold, 1 = a benchmark moved more than
# the threshold, 2 = usage/setup error

set -euo pipefail

# --- Defaults ---
THRESHOLD=5
REPETITIONS=3
DO_BUILD=true
UPDATE_BASELINE=false
BASELINE=""
BUILD_DIR=""
FILTER=""
PINNED_CORES=""

# --- Resolve project root from script location ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# --- Argument parsing ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --threshold)
            THRESHOLD="$2"; shift 2 ;;
        --repetitions)
            REPETITIONS="$2"; shift 2 ;;
        --no-build)
            DO_BUILD=false; shift ;;
        --update-baseline)
            UPDATE_BASELINE=true; shift ;;
        --baseline)
            BASELINE="$2"; shift 2 ;;
        --build-dir)
            BUILD_DIR="$2"; shift 2 ;;
        --filter)
            FILTER="$2"; shift 2 ;;
        --pinned)
            PINNED_CORES="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --threshold N        Regression threshold percentage (default: 5)"
            echo "  --repetitions N      Benchmark repetitions for statistics (default: 3)"
            echo "  --no-build           Skip clean build (use existing build)"
            echo "  --update-baseline    Save current results as new baseline"
            echo "  --baseline FILE      Override baseline file path"
            echo "  --build-dir DIR      Override build directory path"
            echo "  --filter REGEX       Run only matching benchmarks"
            echo "  --pinned CORES       Pin the run to CORES (taskset list, e.g. 3 or 2-3) and"
            echo "                       request real-time priority where permitted"
            echo "  -h, --help           Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

BASELINE="${BASELINE:-$PROJECT_ROOT/benchmark_baseline.json}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
BENCH_EXE="$BUILD_DIR/orderProcessorBench"
COMPARE_PY="$BUILD_DIR/_deps/benchmark-src/tools/compare.py"

# --- Temp files with cleanup ---
CURRENT_RESULTS=$(mktemp /tmp/benchmark_current_XXXXXX.json)
DIFF_RESULTS=$(mktemp /tmp/benchmark_diff_XXXXXX.json)
trap "rm -f '$CURRENT_RESULTS' '$DIFF_RESULTS'" EXIT

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

# --- Step 2: Run benchmarks ---
echo "=== Running Benchmarks (repetitions=$REPETITIONS) ==="
echo "--- machine ---"
echo "  host:      $(uname -n)  kernel $(uname -r)"
echo "  cpu:       $(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')  ($(nproc) threads)"
echo "  governor:  $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
# Report the compiler the benchmark was built with, not whatever $CXX is now.
COMPILER=$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || true)
echo "  compiler:  $("${COMPILER:-${CXX:-c++}}" --version 2>/dev/null | head -1)"
echo "  commit:    $(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo '(not a git checkout)')"

GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)
if [[ "$GOVERNOR" != "performance" && "$GOVERNOR" != "unknown" ]]; then
    echo "  WARNING: CPU governor is '$GOVERNOR'; frequency scaling adds run-to-run noise." >&2
fi

# Build the launcher: taskset pins the run to the given cores, chrt asks for
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
else
    echo "  pinned:    no (pass --pinned CORES for comparable numbers)"
fi
echo ""

BENCH_ARGS=(
    --benchmark_format=json
    "--benchmark_out=$CURRENT_RESULTS"
    "--benchmark_repetitions=$REPETITIONS"
)
if [[ -n "$FILTER" ]]; then
    BENCH_ARGS+=("--benchmark_filter=$FILTER")
fi

"${LAUNCH[@]}" "$BENCH_EXE" "${BENCH_ARGS[@]}"
echo ""

# --- Step 3: If no baseline exists, handle accordingly ---
if [[ ! -f "$BASELINE" ]]; then
    if $UPDATE_BASELINE; then
        cp "$CURRENT_RESULTS" "$BASELINE"
        echo "=== Baseline Created ==="
        echo "Saved to: $BASELINE"
        echo "This baseline belongs to this machine and toolchain; it is deliberately"
        echo "not committed. Re-record it after a toolchain, hardware or kernel change."
        exit 0
    else
        echo "ERROR: No baseline to compare against: $BASELINE" >&2
        echo "" >&2
        echo "Baselines are per-machine and are not committed. Record one first:" >&2
        echo "  $0 --update-baseline${PINNED_CORES:+ --pinned $PINNED_CORES}" >&2
        echo "then make your change and re-run this command." >&2
        exit 2
    fi
fi

# --- Step 4: Check for added/removed benchmarks ---
python3 - "$BASELINE" "$CURRENT_RESULTS" <<'PYEOF'
import json, sys

with open(sys.argv[1]) as f:
    baseline = json.load(f)
with open(sys.argv[2]) as f:
    current = json.load(f)

base_names = {b['name'] for b in baseline['benchmarks'] if b.get('run_type', '') != 'aggregate'}
curr_names = {b['name'] for b in current['benchmarks'] if b.get('run_type', '') != 'aggregate'}
common = base_names & curr_names
added = curr_names - base_names
removed = base_names - curr_names

if added:
    print(f"WARNING: {len(added)} new benchmark(s) not in baseline (will not be compared):")
    for n in sorted(added):
        print(f"  + {n}")

if removed:
    print(f"WARNING: {len(removed)} baseline benchmark(s) missing from current run:")
    for n in sorted(removed):
        print(f"  - {n}")

if common:
    total = len(common) + len(added)
    print(f"\nComparing {len(common)} of {total} benchmarks against baseline.")
else:
    print("ERROR: No benchmarks in common between baseline and current run.", file=sys.stderr)
    sys.exit(2)

if len(common) < len(curr_names) * 0.5:
    print("WARNING: Less than 50% of current benchmarks have a baseline. Consider updating the baseline.")
PYEOF
echo ""

# --- Step 5: Run compare.py ---
echo "=== Comparing Against Baseline ==="
if [[ ! -f "$COMPARE_PY" ]]; then
    echo "ERROR: compare.py not found: $COMPARE_PY" >&2
    echo "Build the project first so FetchContent downloads Google Benchmark." >&2
    exit 2
fi

COMPARE_TOOL_DIR="$(dirname "$COMPARE_PY")"
PYTHONPATH="${COMPARE_TOOL_DIR}:${PYTHONPATH:-}" python3 "$COMPARE_PY" \
    --no-utest \
    --no-color \
    --dump_to_json "$DIFF_RESULTS" \
    benchmarks "$BASELINE" "$CURRENT_RESULTS" 2>/dev/null || true
echo ""

# --- Step 6: Check thresholds and report ---
python3 - "$THRESHOLD" "$DIFF_RESULTS" <<'PYEOF'
import json, sys

threshold_pct = float(sys.argv[1])
threshold_frac = threshold_pct / 100.0

with open(sys.argv[2]) as f:
    diff_report = json.load(f)

regressions = []
improvements = []

for entry in diff_report:
    name = entry['name']
    run_type = entry.get('run_type', '')
    # Skip aggregate rows (mean/median/stddev) — check individual iterations
    if run_type == 'aggregate':
        continue
    for m in entry['measurements']:
        cpu_change = m['cpu']
        if cpu_change > threshold_frac:
            regressions.append({
                'name': name,
                'old_ns': m['cpu_time'],
                'new_ns': m['cpu_time_other'],
                'change_pct': cpu_change * 100,
            })
        elif cpu_change < -threshold_frac:
            improvements.append({
                'name': name,
                'old_ns': m['cpu_time'],
                'new_ns': m['cpu_time_other'],
                'change_pct': cpu_change * 100,
            })

total = len(diff_report)

print("=" * 90)
print(f"  BENCHMARK REGRESSION REPORT  (threshold: {threshold_pct:.1f}%)")
print("=" * 90)

if improvements:
    print(f"\nImprovements ({len(improvements)}):")
    print(f"  {'Benchmark':<55s} {'Old(ns)':>10s} {'New(ns)':>10s} {'Change':>10s}")
    print(f"  {'-'*87}")
    for r in sorted(improvements, key=lambda x: x['change_pct']):
        print(f"  {r['name']:<55s} {r['old_ns']:>10.1f} {r['new_ns']:>10.1f} {r['change_pct']:>+9.1f}%")

if regressions:
    print(f"\nREGRESSIONS ({len(regressions)}):")
    print(f"  {'Benchmark':<55s} {'Old(ns)':>10s} {'New(ns)':>10s} {'Change':>10s}")
    print(f"  {'-'*87}")
    for r in sorted(regressions, key=lambda x: -x['change_pct']):
        print(f"  {r['name']:<55s} {r['old_ns']:>10.1f} {r['new_ns']:>10.1f} {r['change_pct']:>+9.1f}%")
    print(f"\n*** FAIL: {len(regressions)} benchmark(s) regressed >{threshold_pct:.1f}% ***")
    sys.exit(1)
else:
    print(f"\n*** PASS: All benchmarks within {threshold_pct:.1f}% regression threshold ***")
    sys.exit(0)
PYEOF
RESULT=$?

# --- Step 7: Optionally update baseline ---
if $UPDATE_BASELINE && [[ $RESULT -eq 0 ]]; then
    cp "$CURRENT_RESULTS" "$BASELINE"
    echo ""
    echo "=== Baseline Updated ==="
    echo "Saved to: $BASELINE"
fi

exit $RESULT
