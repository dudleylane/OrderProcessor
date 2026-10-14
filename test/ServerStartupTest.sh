#!/usr/bin/env bash
# ServerStartupTest.sh: a server that can't start says why and exits with an error, rather than aborting (#203).
#
# An exception during startup used to reach std::terminate and abort: the reason reached only standard error, the
# server's own log lost it, and in the image every restart dumped core. This runs the server so that it can't start,
# and checks that it exits with status 1, with its reason on standard error and in its log at Fatal. Then it runs it
# twice with an argument it can't use, a usage error (#173), which must exit with status 2 and say why.
#
# usage: ServerStartupTest.sh <orderProcessorServer>

set -u
SERVER="${1:?usage: $0 <orderProcessorServer>}"
WORK="$(mktemp -d)"
trap 'rm -rf -- "${WORK:?}"' EXIT
# A server that aborts anyway mustn't store a core file on the host (systemd-coredump still logs the crash)
ulimit -c 0

failures=0
check() {
    if [[ "$2" == "$3" ]]; then
        echo "ok    $1"
    else
        echo "FAIL  $1: got '$2', wanted '$3'"
        failures=$((failures + 1))
    fi
}

# 1. A data directory the server can't open. A regular file, so that it fails the same way when run as root.
touch "$WORK/not-a-directory"
EXCHANGE_LOG="$WORK/exchange.log" timeout 60 "$SERVER" --port 0 --data-dir "$WORK/not-a-directory" \
    > "$WORK/out1" 2> "$WORK/err1"
status=$?
check "a data directory it can't open: exit status" "$status" 1
check "a data directory it can't open: the reason on standard error" \
    "$(grep -c "Can't start: LMDBStorage::load mdb_env_open: " "$WORK/err1")" 1
check "a data directory it can't open: the reason in the log, at Fatal" \
    "$(grep -c -E ": \[Fatal\]\[[0-9]+\] Can't start: LMDBStorage::load mdb_env_open: " "$WORK/exchange.log" 2>/dev/null)" 1

# 2. A value it can't read: a usage error, before anything starts (#173). The reason and the usage on standard error,
# and exit status 2, as seedData's.
EXCHANGE_LOG="$WORK/exchange2.log" timeout 60 "$SERVER" --port not-a-number > "$WORK/out2" 2> "$WORK/err2"
status=$?
check "a value it can't read: exit status" "$status" 2
check "a value it can't read: the reason on standard error" \
    "$(grep -c -- "--port needs a whole number from 0 to 65535, not 'not-a-number'" "$WORK/err2")" 1
check "a value it can't read: the usage on standard error" "$(grep -c '^usage: orderProcessorServer' "$WORK/err2")" 1

# 3. A flag it doesn't know: the same. Before #173 the server ignored it and ran with the defaults.
EXCHANGE_LOG="$WORK/exchange3.log" timeout 60 "$SERVER" --workres 4 > "$WORK/out3" 2> "$WORK/err3"
status=$?
check "a flag it doesn't know: exit status" "$status" 2
check "a flag it doesn't know: the reason on standard error" "$(grep -c -- "unknown option: --workres" "$WORK/err3")" 1

if (( failures > 0 )); then
    for f in err1 err2 err3; do
        echo "--- standard error ($f):"
        cat "$WORK/$f"
    done
    exit 1
fi
