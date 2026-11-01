#!/usr/bin/env bash
# loop-instructions.sh — the instructions each per-order benchmark's timed span runs, per iteration (#503)
#
# CLAUDE.md's layout check asks whether a change moved the engine's work or only where
# things lie: equal instruction counts per iteration on each slower benchmark. perf stat
# counts the whole process, which holds each benchmark's setup (the resting orders it
# books before its loop) and the store's teardown: #498's whole-process counts were
# mostly those, for the sweeps. This counts only the span bench/ProcessorBench.cpp times
# by hand, EngineFixture::timeEvent(): the event's push and its processing, without the
# span's clock reads.
#
# Each benchmark runs under callgrind for N and for 2N iterations, in a scratch
# directory. The calls into timeEvent() give the span's cost. They are found by the
# function's address, not by where the compiler put the timed loop, and a run that
# didn't call it exactly N or 2N times is an error: it was inlined, or something else
# calls it. The report gives each benchmark's count per iteration averaged over N and
# over 2N, and at the margin, iterations N+1..2N: (2N x avg(2N) - N x avg(N)) / N. The
# margin compares with perf stat's (2N - N) / N, and is where a layout check reads,
# with the averages beside it, since the margin doubles a run's noise.
#
# Usage:
#   ./scripts/loop-instructions.sh [--filter REGEX] [--iterations N] BINARY [BINARY]
#     --filter REGEX    the benchmarks, as --benchmark_filter takes them (^BM_Process)
#     --iterations N    N, run as N and as 2N iterations (2000)
# With two binaries, the benchmarks both list are counted in each, and the report adds
# the second's counts less the first's. Both must time their span with timeEvent(), as
# every build since #503 does. The 18 BM_Process* at N = 2000 take about two minutes a
# binary under callgrind.
#
# Exit codes: 0 = every count made, 2 = usage or setup error, or a run or a count failed

set -euo pipefail

FILTER='^BM_Process'
N=2000
BINARIES=()

usage_error() {
    echo "loop-instructions: $1" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --filter|--iterations)
            [[ $# -ge 2 ]] || usage_error "$1 needs a value"
            if [[ "$1" == --filter ]]; then FILTER="$2"; else N="$2"; fi
            shift 2 ;;
        -h|--help)
            sed -n '2,/^$/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
            exit 0 ;;
        -*)
            usage_error "unknown option '$1'" ;;
        *)
            BINARIES+=("$1"); shift ;;
    esac
done
(( ${#BINARIES[@]} == 1 || ${#BINARIES[@]} == 2 )) || usage_error "give one benchmark binary or two"
[[ "$N" =~ ^[1-9][0-9]*$ ]] || usage_error "--iterations takes a positive whole number, not '$N'"
for tool in valgrind nm python3 sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || usage_error "$tool is not installed"
done
for i in "${!BINARIES[@]}"; do
    [[ -f "${BINARIES[i]}" && -x "${BINARIES[i]}" ]] || usage_error "not an executable: ${BINARIES[i]}"
    BINARIES[i]=$(realpath "${BINARIES[i]}")
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/loop-instructions.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# The benchmarks to count: those the first binary lists, and with two, those the second lists too
"${BINARIES[0]}" --benchmark_list_tests --benchmark_filter="$FILTER" > "$WORK/names.0" ||
    usage_error "${BINARIES[0]} can't list its benchmarks"
cp "$WORK/names.0" "$WORK/names"
if (( ${#BINARIES[@]} == 2 )); then
    "${BINARIES[1]}" --benchmark_list_tests --benchmark_filter="$FILTER" > "$WORK/names.1" ||
        usage_error "${BINARIES[1]} can't list its benchmarks"
    grep -Fxf "$WORK/names.1" "$WORK/names.0" > "$WORK/names" || true
    while read -r name; do
        grep -Fqx "$name" "$WORK/names" || echo "loop-instructions: only one binary has $name; not counted" >&2
    done < <(cat "$WORK/names.0" "$WORK/names.1")
fi
[[ -s "$WORK/names" ]] || usage_error "no benchmark matches '$FILTER'"

# The span's instructions per iteration in one callgrind profile of N iterations: what the calls into timeEvent() cost,
# less the clock reads and SetIterationTime() it calls
count() {
    python3 - "$1" "$2" "$3" <<'PYEOF'
import subprocess
import sys

profile, binary, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
SPAN = 'EngineFixture::timeEvent<'
CLOCK = ('steady_clock::now()', 'State::SetIterationTime(')
# Under LTO a call can go through a local alias of its function, so calls into timeEvent() are matched by their target
# address: each instantiation's, and any clone's
span_at = set()
for line in subprocess.run(['nm', '-C', binary], capture_output=True, text=True, check=True).stdout.splitlines():
    parts = line.split(' ', 2)
    if len(parts) == 3 and parts[1] in ('T', 't', 'W', 'w') and SPAN in parts[2]:
        span_at.add(int(parts[0], 16))
if not span_at:
    sys.exit(f'{binary} has no {SPAN}...>(): it predates #503')
fn = cfn = pending = None
calls = cost = clock = 0
with open(profile) as f:
    for line in f:
        line = line.rstrip('\n')
        if line.startswith('fn='):
            fn, cfn, pending = line[3:], None, None
        elif line.startswith('cfn='):
            cfn = line[4:]
        elif line.startswith('calls='):
            count, target = line[6:].split()[:2]
            pending = (int(count), int(target, 16))
        elif pending is not None:
            inclusive = int(line.split()[1])
            if pending[1] in span_at:
                calls += pending[0]
                cost += inclusive
            elif fn is not None and SPAN in fn and cfn is not None and any(c in cfn for c in CLOCK):
                clock += inclusive
            pending = None
if calls != n:
    sys.exit(f'timeEvent() was called {calls} times in {n} iterations: inlined, or called outside the timed loop')
print(f'{(cost - clock) / n:.1f}')
PYEOF
}

: > "$WORK/counts"
failed=0
echo "== loop-instructions (#503): instructions per iteration in timeEvent()'s span, at N = $N and 2N = $((2 * N))"
for i in "${!BINARIES[@]}"; do
    echo "   $((i + 1)): ${BINARIES[i]} (sha256 $(sha256sum < "${BINARIES[i]}" | cut -c1-12))"
done
k=0
while read -r name; do
    k=$((k + 1))
    # The name as a regular expression, its metacharacters escaped
    # shellcheck disable=SC2001 # the escape repeats each match (&), which ${name//} does only from bash 5.2
    pattern="^$(sed -e 's/[].[*^$+?(){}|\\]/\\&/g' <<< "$name")\$"
    for i in "${!BINARIES[@]}"; do
        for n in "$N" "$((2 * N))"; do
            profile="$WORK/$i.$k.$n.cg"
            if ! (cd "$WORK" && valgrind --tool=callgrind --dump-instr=yes --dump-line=no --compress-pos=no \
                    --compress-strings=no --callgrind-out-file="$profile" "${BINARIES[i]}" \
                    --benchmark_filter="$pattern" --benchmark_min_time="${n}x") > "$profile.out" 2> "$profile.err"; then
                echo "loop-instructions: $name in binary $((i + 1)) failed at $n iterations:" >&2
                tail -n 5 "$profile.err" >&2
                failed=1
                continue
            fi
            if value=$(count "$profile" "${BINARIES[i]}" "$n" 2>&1); then
                printf '%s\t%s\t%s\t%s\n' "$i" "$name" "$n" "$value" >> "$WORK/counts"
            else
                echo "loop-instructions: $name in binary $((i + 1)) at $n iterations: $value" >&2
                failed=1
            fi
            rm -f "$profile"
        done
    done
done < "$WORK/names"

python3 - "$WORK/counts" "$WORK/names" "$N" "${#BINARIES[@]}" <<'PYEOF'
import sys

counts, names, n, binaries = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
v = {}
for line in open(counts):
    i, name, iterations, value = line.rstrip('\n').split('\t')
    v[(int(i), name, int(iterations))] = float(value)


def triple(i, name):
    a, b = v.get((i, name, n)), v.get((i, name, 2 * n))
    if a is None or b is None:
        return None
    return a, b, (2 * n * b - n * a) / n


head = f"{'benchmark':48s}" + ''.join(f"{f'{i + 1}: N':>10s}{'2N':>10s}{'margin':>10s}" for i in range(binaries))
if binaries == 2:
    head += f"{'2-1: N':>10s}{'2N':>8s}{'margin':>8s}"
print(head)
for name in open(names).read().split():
    row = f'{name:48s}'
    t = [triple(i, name) for i in range(binaries)]
    for x in t:
        row += ''.join(f'{y:10,.0f}' for y in x) if x else f"{'failed':>30s}"
    if binaries == 2 and None not in t:
        row += f'{t[1][0] - t[0][0]:+10.0f}{t[1][1] - t[0][1]:+8.0f}{t[1][2] - t[0][2]:+8.0f}'
    print(row)
PYEOF
exit $(( failed ? 2 : 0 ))
