#!/usr/bin/env bash
# scripts/benchmark-regression.sh's verdict when a pinned run starts with a watched CPU busy (#422): INCONCLUSIVE,
# exit 3, naming each such run, ahead of a FAIL, as ruled. The script samples its CPUs through the cpu-busy.sh beside
# it, so this runs a copy of it beside a stand-in that gives each run the sample a case sets, with stub benchmark
# binaries that take no time.
# usage: BenchmarkBusyStartTest.sh <scripts/benchmark-regression.sh>
set -u
SCRIPT=$(realpath -e -- "$1") || { echo "no script $1"; exit 1; }
command -v python3 > /dev/null || { echo "python3 is missing, as the script needs it: skipped"; exit 77; }
command -v taskset > /dev/null || { echo "taskset is missing, as a pinned comparison needs it: skipped"; exit 77; }
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

# The stand-in watches the CPUs it is given, and answers each run's sample with the next line of STUB_SAMPLES. The
# script's own check that it can sample at all, before anything runs, reads /proc/stat twice and takes no line.
mkdir -p "$work/tool/scripts"
cp -- "$SCRIPT" "$work/tool/scripts/benchmark-regression.sh"
export STUB_SAMPLES="$work/samples"
cat > "$work/tool/scripts/cpu-busy.sh" << 'EOF'
#!/usr/bin/env bash
case "$1" in
    watched) echo "$2" ;;
    busiest)
        if [[ /proc/stat == "$3" ]]; then
            echo "0 0"
            exit 0
        fi
        sample=$(head -n 1 "$STUB_SAMPLES")
        sed -i 1d "$STUB_SAMPLES"
        echo "${sample:-0 0}"
        ;;
    *) exit 2 ;;
esac
EOF
chmod +x "$work/tool/scripts/cpu-busy.sh"

# A build of a tree without config/, whose benchmark binary is a stub that lists one benchmark and reports it at the
# given time: 1000 ns, over --min-time, so that a slower one can fail
make_build() { # <name> <ns>
    local build="$work/$1-build"
    mkdir -p "$work/$1-src" "$build"
    echo "CMAKE_HOME_DIRECTORY:INTERNAL=$work/$1-src" > "$build/CMakeCache.txt"
    sed "s|@NS@|$2|g" > "$build/orderProcessorBench" << 'EOF'
#!/usr/bin/env bash
out=""
for arg in "$@"; do
    case "$arg" in
        --benchmark_list_tests=true) echo "BM_Stub"; exit 0 ;;
        --benchmark_out=*) out=${arg#--benchmark_out=} ;;
    esac
done
echo '{"benchmarks": [{"name": "BM_Stub", "run_name": "BM_Stub", "run_type": "iteration", "real_time": @NS@, "cpu_time": @NS@, "time_unit": "ns"}]}' > "$out"
EOF
    chmod +x "$build/orderProcessorBench"
}
make_build before 1000
make_build same 1000
make_build slower 2000
base="$work/base.bin"
if ! bash "$work/tool/scripts/benchmark-regression.sh" --update-baseline --no-build --build-dir "$work/before-build" \
    --baseline "$base" > "$work/out" 2>&1; then
    cat "$work/out"
    echo "can't save the baseline"
    exit 1
fi

# Two rounds, four runs, sampled in the order they run: round 1's before and after, then round 2's after and before.
# --settle-max 0 takes one sample a run, the one it starts on.
samples() { printf '%s\n' "$@" > "$STUB_SAMPLES"; }
left() { grep -c . "$STUB_SAMPLES"; }
compare() { # <build> [option...]
    local build=$1
    shift
    bash "$work/tool/scripts/benchmark-regression.sh" --no-build --rounds 2 --settle-max 0 --memory-cap 0 \
        --build-dir "$work/$build-build" --baseline "$base" "$@" > "$work/out" 2>&1
}
named() { grep -c '^  round [0-9]*, [a-z]*: CPU ' "$work/out"; }

# --- Every run starts quiet: the numbers decide ---
samples "0 5" "0 5" "0 5" "0 5"
compare same --pinned 0
check "quiet starts pass" 0 $?
check "each run took one sample" 0 "$(left)"
check "the report gives the busiest" 1 "$(grep -c 'busiest watched CPU (of 0) was 5..5% busy' "$work/out")"
check "and names no run" 0 "$(named)"

# --- Round 2's baseline run, the last to run, starts busy: inconclusive, naming that run alone ---
samples "0 5" "0 5" "0 5" "0 95"
compare same --pinned 0
check "a busy start is inconclusive" 3 $?
check "it says so" 1 "$(grep -c '^\*\*\* INCONCLUSIVE: 1 of the 4 runs started with a watched CPU 20% busy or more' \
    "$work/out")"
check "naming the run" 1 "$(grep -c '^  round 2, before: CPU 0 95% busy$' "$work/out")"
check "and it alone" 1 "$(named)"

# --- A slower build: a busy start comes ahead of the FAIL its numbers give ---
samples "0 5" "0 5" "0 5" "0 5"
compare slower --pinned 0
check "quiet starts of a slower build fail" 1 $?
samples "0 5" "0 60" "0 5" "0 5"
compare slower --pinned 0
check "a busy start of a slower build is inconclusive" 3 $?
check "naming round 1's current build" 1 "$(grep -c '^  round 1, after: CPU 0 60% busy$' "$work/out")"
check "and not saying FAIL" 0 "$(grep -c '^\*\*\* FAIL' "$work/out")"

# --- The mark: a run waits for its CPUs to be under --settle-busy, so 20% is busy and 19% isn't ---
samples "0 19" "0 19" "0 19" "0 19"
compare same --pinned 0
check "19% busy is quiet" 0 $?
samples "0 19" "0 20" "0 19" "0 19"
compare same --pinned 0
check "20% busy is busy" 3 $?
samples "0 19" "0 20" "0 19" "0 19"
compare same --pinned 0 --settle-busy 21
check "under a --settle-busy of 21 it isn't" 0 $?

# --- Unpinned, no CPU is watched or sampled, and the numbers decide ---
samples "0 95" "0 95" "0 95" "0 95"
compare same
check "an unpinned comparison passes" 0 $?
check "and takes no sample" 4 "$(left)"

if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
