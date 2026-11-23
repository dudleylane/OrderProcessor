#!/usr/bin/env bash
# seedData --check restores a directory as a server's start does, with the environment read-only (#591): it passes a
# seeded directory and leaves its data.mdb's bytes as they were, refuses a directory with no data.mdb (exit 1) and
# writes nothing into it, and refuses --check with an option that writes as a usage error (exit 2). ServerDataTest's
# cases cover the restore itself, and a record it refuses.
#
# usage: SeedDataCheckTest.sh <seedData>
set -euo pipefail

seed=$1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
fail() {
    echo "FAIL: $*" >&2
    exit 1
}

"$seed" "$dir/data" > "$dir/seed.out" 2>&1 || fail "seedData couldn't seed a new directory: $(cat "$dir/seed.out")"
before=$(sha256sum < "$dir/data/data.mdb")
out=$("$seed" --check "$dir/data" 2>&1) || fail "--check refused a seeded directory: $out"
[[ "$out" == *"every record in $dir/data restores: "* ]] || fail "--check didn't say what it found: $out"
[[ "$(sha256sum < "$dir/data/data.mdb")" == "$before" ]] || fail "--check changed data.mdb"

mkdir "$dir/empty"
status=0
out=$("$seed" --check "$dir/empty" 2>&1) || status=$?
[[ $status -eq 1 && "$out" == *"holds no data.mdb"* ]] \
    || fail "--check of a directory with no data.mdb exited $status: $out"
[[ -z "$(ls -A "$dir/empty")" ]] || fail "--check wrote into the directory it refused: $(ls -A "$dir/empty")"

status=0
out=$("$seed" --check --if-empty "$dir/data" 2>&1) || status=$?
[[ $status -eq 2 ]] || fail "--check --if-empty exited $status, where a usage error exits 2: $out"
[[ "$(sha256sum < "$dir/data/data.mdb")" == "$before" ]] || fail "a refused --check changed data.mdb"

echo "PASS: seedData --check"
