#!/usr/bin/env bash
# scripts/benchmark-regression.sh's baseline and the config it reads (#415), with stub benchmark binaries that read
# their source tree's config/fix.cfg as feature/fx-swap-support's FIX session benchmarks do: --update-baseline saves a
# copy of that config/ beside the baseline, a comparison hands the copy to the baseline and nothing to the current
# build, and a benchmark that reports an error in the first round stops the comparison there.
# usage: BenchmarkBaselineTest.sh <scripts/benchmark-regression.sh>
set -u
SCRIPT=$(realpath -e -- "$1") || { echo "no script $1"; exit 1; }
command -v python3 > /dev/null || { echo "python3 is missing, as the script needs it: skipped"; exit 77; }
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

# The script runs from a checkout of its own, with no config/, so nothing can come from there by mistake
mkdir -p "$work/tool/scripts"
cp -- "$SCRIPT" "$work/tool/scripts/benchmark-regression.sh"
compare() { bash "$work/tool/scripts/benchmark-regression.sh" "$@" 2>&1; }

# A source tree, and a build of it: its cache names the tree, and its benchmark binary is a stub built from it. The stub
# lists one benchmark. A run records in runs.log which binary ran and the config it read, as BenchSession reads it: from
# OP_BENCH_CONFIG_DIR when that is set, else from the tree built into it, or none for a tree without config/ (master).
# Then it reports the benchmark at 100 ns, or failed when that config isn't there.
export STUB_LOG="$work/runs.log"
make_build() { # <name> <reads config: yes|no>
    local src="$work/$1-src" build="$work/$1-build" built=""
    mkdir -p "$src" "$build"
    if [[ yes == "$2" ]]; then
        mkdir -p "$src/config/spec"
        echo "the $1 tree's fix.cfg" > "$src/config/fix.cfg"
        echo "the $1 tree's dictionary" > "$src/config/spec/FIXT11.xml"
        built=$src
    fi
    echo "CMAKE_HOME_DIRECTORY:INTERNAL=$src" > "$build/CMakeCache.txt"
    sed "s|@SRC@|$built|" > "$build/orderProcessorBench" << 'EOF'
#!/usr/bin/env bash
src='@SRC@'
out=""
for arg in "$@"; do
    case "$arg" in
        --benchmark_list_tests=true) echo "BM_Stub"; exit 0 ;;
        --benchmark_out=*) out=${arg#--benchmark_out=} ;;
    esac
done
config=""
if [[ -n "$src" ]]; then
    config="${OP_BENCH_CONFIG_DIR:-$src/config}/fix.cfg"
fi
echo "$(basename "$0") ${config:-none}" >> "$STUB_LOG"
if [[ -n "$config" && ! -f "$config" ]]; then
    printf '{"benchmarks": [{"name": "BM_Stub", "run_name": "BM_Stub", "run_type": "iteration", "error_occurred": true, "error_message": "File %s not found"}]}\n' "$config" > "$out"
else
    echo '{"benchmarks": [{"name": "BM_Stub", "run_name": "BM_Stub", "run_type": "iteration", "real_time": 100, "cpu_time": 100, "time_unit": "ns"}]}' > "$out"
fi
EOF
    chmod +x "$build/orderProcessorBench"
}
make_build before yes
make_build after yes
base="$work/base.bin"
runs() { grep -c . "$STUB_LOG" 2>/dev/null || echo 0; }
read_by() { grep "^$1 " "$STUB_LOG" | cut -d' ' -f2 | sort -u | tr '\n' ' '; } # <binary name>
# No settle and no memory cap: the stubs take no time and no memory
quick=(--no-build --rounds 2 --settle-max 0 --memory-cap 0)

# --- Saving a baseline ---
compare --update-baseline --no-build --build-dir "$work/before-build" --baseline "$base" > "$work/out"
check "--update-baseline succeeds" 0 $?
check "the baseline's config/ is saved beside it" "the before tree's fix.cfg" "$(cat "$base.config/fix.cfg" 2>&1)"
check "with its dictionaries" "the before tree's dictionary" "$(cat "$base.config/spec/FIXT11.xml" 2>&1)"
check "the .info names the copy" "config:    $base.config, a copy of $work/before-src/config" \
    "$(grep '^config:' "$base.info")"

# --- A comparison after the baseline's tree is gone, as when its worktree is removed once its branch merges ---
rm -rf -- "${work:?}/before-src"
: > "$STUB_LOG"
OP_BENCH_CONFIG_DIR="$work/the-caller's" compare "${quick[@]}" --build-dir "$work/after-build" --baseline "$base" \
    > "$work/out"
check "the comparison passes without the baseline's tree" 0 $?
check "both sides ran in both rounds" 4 "$(runs)"
check "the baseline reads the copy saved with it" "$base.config/fix.cfg " "$(read_by base.bin)"
check "the current build reads its own tree" "$work/after-src/config/fix.cfg " "$(read_by orderProcessorBench)"
check "the header names the copy" 1 "$(grep -c "config:    $base.config, a copy of" "$work/out")"

# --- A baseline saved without its config, as before #415: its benchmark fails, and the comparison stops at once ---
rm -rf -- "${base:?}.config"
: > "$STUB_LOG"
OP_BENCH_CONFIG_DIR="$work/the-caller's" compare "${quick[@]}" --rounds 20 --build-dir "$work/after-build" \
    --baseline "$base" > "$work/out"
check "a benchmark that can't run is an error" 2 $?
check "it stops after the first round" 2 "$(runs)"
check "it says so" 1 "$(grep -c 'did not produce a measurement in round 1, so the comparison stops there' "$work/out")"
check "naming the benchmark, the side and its error" 1 \
    "$(grep -cF "! BM_Stub: before: File $work/before-src/config/fix.cfg not found" "$work/out")"
check "the caller's OP_BENCH_CONFIG_DIR reaches neither side" "$work/before-src/config/fix.cfg " "$(read_by base.bin)"
check "the header says none was saved" 1 "$(grep -c 'config:    none saved with it (#415)' "$work/out")"

# --- A tree without config/, as on master: nothing is saved, and a copy from an earlier save at that path goes ---
make_build master no
mkdir -p "$base.config"
echo stale > "$base.config/fix.cfg"
compare --update-baseline --no-build --build-dir "$work/master-build" --baseline "$base" > "$work/out"
check "--update-baseline succeeds without a config/" 0 $?
check "no copy is left beside the baseline" no "$([[ -e "$base.config" ]] && echo yes || echo no)"
check "the .info names no config" 0 "$(grep -c '^config:' "$base.info")"
: > "$STUB_LOG"
OP_BENCH_CONFIG_DIR="$work/the-caller's" compare "${quick[@]}" --build-dir "$work/master-build" --baseline "$base" \
    > "$work/out"
check "a comparison of two builds that read no config passes" 0 $?
check "and its header has no config line" 0 "$(grep -c 'config:' "$work/out")"

if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
