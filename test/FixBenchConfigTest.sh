#!/usr/bin/env bash
# Where the FIX session benchmarks read their config (#415): BM_FixSendExecutionReport logs its session on from
# fix.cfg and the dictionaries under spec/ in OP_BENCH_CONFIG_DIR when that is set, as benchmark-regression.sh sets it
# to the copy saved with a baseline, and in the config/ of the tree the binary was built from otherwise. The binary must
# exit 0 each time, as it does after a SkipWithError, so that in CI's ASan job a sanitizer's report on these paths fails
# the test.
# usage: FixBenchConfigTest.sh <orderProcessorBench> <the source tree's config/>
set -u
BENCH=$(realpath -e -- "$1") || { echo "no benchmark binary $1"; exit 1; }
CONFIG=$(realpath -e -- "$2") || { echo "no config directory $2"; exit 1; }
command -v python3 > /dev/null || { echo "python3 is missing: skipped"; exit 77; }
work=$(mktemp -d)
trap 'rm -rf -- "${work:?}"' EXIT
failures=0

check() { # <what> <expected> <actual>
    if [[ "$2" == "$3" ]]; then
        echo "ok: $1"
    else
        echo "FAILED: $1: expected '$2', got '$3'"
        failures=$(( failures + 1 ))
    fi
}

# One iteration of the benchmark, with the given env arguments, in a scratch directory, since the binary writes its
# log where it runs. Prints the benchmark's error, or ok, or how the binary exited if that wasn't 0. The log's end goes
# to standard error then, where no check reads it, since it holds the benchmark's error too.
run() { # <env arguments>...
    rm -f -- "$work/out.json"
    ( cd "$work" && env "$@" "$BENCH" --benchmark_filter='^BM_FixSendExecutionReport$' --benchmark_min_time=1x \
        --benchmark_out="$work/out.json" --benchmark_out_format=json > "$work/log" 2>&1 )
    local status=$?
    if (( 0 != status )); then
        tail -n 20 -- "$work/log" >&2
        echo "the benchmark binary exited $status"
        return
    fi
    python3 - "$work/out.json" << 'PYEOF'
import json, sys
try:
    with open(sys.argv[1]) as f:
        runs = json.load(f).get('benchmarks', [])
except (OSError, ValueError) as e:
    print(f'no report: {e}')
    sys.exit(0)
errors = [b.get('error_message') or 'error' for b in runs if b.get('error_occurred')]
print(errors[0] if errors else ('ok' if runs else 'no benchmark ran'))
PYEOF
}
contains() { [[ "$1" == *"$2"* ]] && echo yes || echo "no: $1"; } # <text> <part>

mkdir -p "$work/empty" "$work/no-dictionaries"
cp -R -- "$CONFIG" "$work/copy"
cp -- "$CONFIG/fix.cfg" "$work/no-dictionaries/fix.cfg"

check "without the variable, it reads the tree it was built from" ok "$(run -u OP_BENCH_CONFIG_DIR)"
check "an empty variable is none" ok "$(run OP_BENCH_CONFIG_DIR=)"
check "it reads a copy the variable names" ok "$(run OP_BENCH_CONFIG_DIR="$work/copy")"
check "a directory without fix.cfg fails, naming the file it tried" yes \
    "$(contains "$(run OP_BENCH_CONFIG_DIR="$work/empty")" "no session from $work/empty/fix.cfg: ")"
check "the dictionaries come from the same directory" yes \
    "$(contains "$(run OP_BENCH_CONFIG_DIR="$work/no-dictionaries")" "$work/no-dictionaries/spec/")"

if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
