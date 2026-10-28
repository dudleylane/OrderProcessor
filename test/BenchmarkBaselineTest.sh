#!/usr/bin/env bash
# scripts/benchmark-regression.sh's baseline and the config it reads (#415), with stub benchmark binaries that read
# their source tree's config/fix.cfg as feature/fx-swap-support's FIX session benchmarks do: --update-baseline saves a
# copy of that config/ beside the baseline, a comparison hands the copy to the baseline and nothing to the current
# build, and a benchmark that reports an error in the first round stops the comparison there. Each report's commit:
# lines, the baseline's .info's among them, name the commit of the tree each binary was built from, which needn't be
# the script's checkout (#448).
# usage: BenchmarkBaselineTest.sh <scripts/benchmark-regression.sh>
set -u
SCRIPT=$(realpath -e -- "$1") || { echo "no script $1"; exit 1; }
command -v python3 > /dev/null || { echo "python3 is missing, as the script needs it: skipped"; exit 77; }
command -v git > /dev/null || { echo "git is missing, as the trees' commits need it: skipped"; exit 77; }
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR GIT_PREFIX # a hook's, which would name another repository
work=$(mktemp -d)
trap 'rm -rf -- "${work:?}"' EXIT
export GIT_CEILING_DIRECTORIES="$work" # a tree under it is a checkout of its own or none, whatever $work lies in
failures=0

check() { # <what> <expected> <actual>
    if [[ "$2" == "$3" ]]; then
        echo "ok: $1"
    else
        echo "FAILED: $1: expected '$2', got '$3'"
        failures=$(( failures + 1 ))
    fi
}

# Makes a directory a git checkout with one commit of what it holds, and prints that commit as the script names it
commit_tree() { # <dir> <message>
    git -c init.defaultBranch=main -C "$1" init -q &&
        git -C "$1" add -A &&
        git -C "$1" -c user.name=test -c user.email=test@example.invalid -c commit.gpgsign=false \
            -c core.hooksPath=/dev/null commit -q --allow-empty -m "$2" &&
        git -C "$1" rev-parse --short HEAD
}

# The script runs from a checkout of its own, with no config/, so nothing can come from there by mistake
mkdir -p "$work/tool/scripts"
cp -- "$SCRIPT" "$work/tool/scripts/benchmark-regression.sh"
tool_sha=$(commit_tree "$work/tool" "the script's checkout") || { echo "can't make the script's checkout"; exit 1; }
compare() { bash "$work/tool/scripts/benchmark-regression.sh" "$@" 2>&1; }

# A source tree, a git checkout with one commit, and a build of it: its cache names the tree, and its benchmark binary
# is a stub built from it. The stub lists one benchmark. A run records in runs.log which binary ran and the config it read, as BenchSession reads it: from
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
    echo "the $1 tree's sources" > "$src/sources.txt"
    commit_tree "$src" "the $1 tree" > "$work/$1.sha" || { echo "can't make the $1 tree a git checkout"; exit 1; }
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
sha_of() { cat "$work/$1.sha"; } # <name>: the commit of that tree, as the script names it
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
check "the .info names the commit of the tree the baseline was built from, not the script's (#448)" \
    "commit:    $(sha_of before) in $work/before-src" "$(grep '^commit:' "$base.info")"

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
check "the header gives the baseline's commit as it was saved" 1 \
    "$(grep -cxF "  commit:    $(sha_of before) in $work/before-src" "$work/out")"
check "and the current build's, of the tree it was built from" 1 \
    "$(grep -cxF "  commit:    $(sha_of after) in $work/after-src" "$work/out")"

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

# --- Each build's commit line, whatever its tree's state (#448) ---
current_commit() { # <build>: the comparison's commit: line for the current build
    compare "${quick[@]}" --build-dir "$work/$1" --baseline "$base" > "$work/out"
    sed -n '/^--- after/,/^  commit:/s/^  commit:    //p' "$work/out"
}
echo "changed" > "$work/master-src/sources.txt"
check "a tree with uncommitted changes says so" "$(sha_of master) with uncommitted changes in $work/master-src" \
    "$(current_commit master-build)"
mkdir -p "$work/tool-build"
cp -- "$work/master-build/orderProcessorBench" "$work/tool-build/"
echo "CMAKE_HOME_DIRECTORY:INTERNAL=$work/tool" > "$work/tool-build/CMakeCache.txt"
check "a build of the script's own checkout is named by its commit alone" "$tool_sha" "$(current_commit tool-build)"
rm -rf -- "${work:?}/master-src"
check "a tree that is gone is said to be" "unknown: $work/master-src, the tree it was built from, is gone" \
    "$(current_commit master-build)"
mkdir -p "$work/master-src"
check "a tree that isn't a git checkout is said not to be" "unknown: $work/master-src is not a git checkout" \
    "$(current_commit master-build)"
rm -f -- "$work/master-build/CMakeCache.txt"
check "a build whose cache names no tree is said to" \
    "unknown: $work/master-build/CMakeCache.txt doesn't name the tree it was built from" \
    "$(current_commit master-build)"

if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
