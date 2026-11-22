#!/usr/bin/env bash
# scripts/line-endings.sh (#639), against a made-up repository: C++ and CMake files must be CRLF, the shell scripts,
# the pre-push hook, .github/ and README.md LF, and no file may mix the two, as committed; anything else passes as it
# is. A directory that isn't a git work tree is refused.
# usage: LineEndingsTest.sh <scripts/line-endings.sh>
set -u
SCRIPT=$(realpath -e -- "$1") || { echo "no script $1"; exit 1; }
command -v git > /dev/null || { echo "git is missing, as the script needs it: skipped"; exit 77; }
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

crlf() { printf 'one\r\ntwo\r\n' > "$1"; }
lf() { printf 'one\ntwo\n' > "$1"; }

repo=$work/repo
mkdir -p "$repo/src" "$repo/scripts/hooks" "$repo/.github/workflows" "$repo/docs"
git -C "$repo" init -q
git -C "$repo" config core.autocrlf false
crlf "$repo/src/Good.cpp"
crlf "$repo/src/Good.h"
crlf "$repo/CMakeLists.txt"
crlf "$repo/src/CMakeLists.txt"
lf "$repo/scripts/good.sh"
lf "$repo/scripts/hooks/pre-push"
lf "$repo/.github/workflows/good.yml"
lf "$repo/README.md"
lf "$repo/docs/notes.md"     # neither kind: passes either way
crlf "$repo/docs/other.md"   # neither kind
: > "$repo/src/Empty.sh"     # no line ending at all
git -C "$repo" add -A

out=$(bash "$SCRIPT" "$repo" 2>&1)
check "a repository that keeps the rule passes" "0" "$?"

lf "$repo/src/Bad.cpp"
lf "$repo/src/Bad.h"
lf "$repo/src/cmake.cmake"
crlf "$repo/scripts/bad.sh"
crlf "$repo/.github/workflows/bad.yml"
printf 'one\r\ntwo\n' > "$repo/docs/mixed.md"
git -C "$repo" add -A
out=$(bash "$SCRIPT" "$repo" 2>&1)
check "a repository that breaks the rule fails" "1" "$?"
for name in src/Bad.cpp src/Bad.h src/cmake.cmake scripts/bad.sh .github/workflows/bad.yml docs/mixed.md; do
    check "names $name" "1" "$(grep -c -- " $name\$" <<< "$out")"
done
for name in src/Good.cpp src/Good.h CMakeLists.txt src/CMakeLists.txt scripts/good.sh scripts/hooks/pre-push \
    .github/workflows/good.yml README.md docs/notes.md docs/other.md src/Empty.sh; do
    check "doesn't name $name" "0" "$(grep -c -- " $name\$" <<< "$out")"
done

# What's committed decides, not the working tree: a CRLF file staged, then rewritten as LF on disk, still passes
git -C "$repo" rm -q --cached src/Bad.cpp src/Bad.h src/cmake.cmake scripts/bad.sh .github/workflows/bad.yml \
    docs/mixed.md
crlf "$repo/src/Late.cpp"
git -C "$repo" add src/Late.cpp
lf "$repo/src/Late.cpp"
out=$(bash "$SCRIPT" "$repo" 2>&1)
check "the index decides, not the working tree" "0" "$?"

out=$(bash "$SCRIPT" "$work" 2>&1)
check "a directory that isn't a work tree is refused" "2" "$?"

if (( failures > 0 )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
