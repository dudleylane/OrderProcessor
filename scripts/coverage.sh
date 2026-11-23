#!/usr/bin/env bash
# coverage.sh: the test suite's line coverage of src/ and app/ (#592). Information, not a gate: no figure fails anything.
#
# A Debug build with COVERAGE=ON, which compiles this project's own targets with --coverage and nothing else, then
# ctest, then lcov's capture of the counters, kept to src/ and app/, its summary, and genhtml's report. gcov must be the
# compiler's own, as gcc-toolset-15's PATH makes it: another GCC's gcov can't read the counters, so the script checks.
#
# usage: scripts/coverage.sh [BUILD_DIR]   (default: build-coverage/ in this repository)
#
# Prints lcov's summary and where genhtml's report is. Exits 2 if a tool is missing or a step fails, 1 if a test
# failed (the figures are then of the run as it went), 0 otherwise.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(realpath -m -- "${1:-$ROOT/build-coverage}")"

refuse() {
    echo "coverage: $*" >&2
    exit 2
}

for tool in cmake ctest lcov genhtml gcov; do
    command -v "$tool" > /dev/null || refuse "$tool is missing"
done
cxx="${CXX:-c++}"
command -v "$cxx" > /dev/null || refuse "the compiler, $cxx, is missing"
gcc_major="$("$cxx" -dumpversion | cut -d. -f1)"
# gcov takes no -dumpversion: it reads it as its short options and prints its banner, "gcov (GCC) 15.2.1 ...", so the
# major version is read from that banner
gcov_major="$(gcov --version | sed -nE '1s/.*\) ([0-9]+)\..*/\1/p')"
[[ "$gcc_major" == "$gcov_major" ]] ||
    refuse "gcov is GCC $gcov_major's and $cxx is GCC $gcc_major's: put the compiler's own gcov first in PATH"
jobs="$("$ROOT/scripts/ci-jobs.sh" 2> /dev/null || nproc)"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCOVERAGE=ON -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=OFF \
    -DBUILD_APP=ON || refuse "configure failed"
cmake --build "$BUILD" -j"$jobs" || refuse "build failed"

lcov --quiet --zerocounters --directory "$BUILD" || refuse "lcov couldn't zero the counters"
status=0
ctest --test-dir "$BUILD" -j"$jobs" --timeout 600 > "$BUILD/coverage-ctest.log" 2>&1 || status=1
# Only src/ and app/ are read, at the capture: geninfo checks each file it reads, and refused the whole capture over a
# GoogleTest body's end line in a test file ("mismatched end line"), which the report leaves out anyway
lcov --quiet --capture --directory "$BUILD" --include "$ROOT/src/*" --include "$ROOT/app/*" \
    --output-file "$BUILD/coverage.info" || refuse "lcov's capture failed"
lcov --summary "$BUILD/coverage.info"
genhtml --quiet "$BUILD/coverage.info" --output-directory "$BUILD/coverage-html" || refuse "genhtml failed"
echo "report: $BUILD/coverage-html/index.html"
if ((status != 0)); then
    echo "coverage: a test failed (see $BUILD/coverage-ctest.log): the figures are of the run as it went" >&2
fi
exit "$status"
