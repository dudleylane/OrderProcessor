#!/usr/bin/env bash
# SeedDataIfEmptyTest.sh: seedData --if-empty seeds a directory with no instrument, and leaves one with instruments alone
# (#161). The case it is for: a directory a server ran on before it was seeded, as the quick start used to leave one,
# has an LMDB file but no instrument, so a check for the file would never seed it.
#
# usage: SeedDataIfEmptyTest.sh <seedData> <orderProcessorServer>

set -u
SEED="${1:?usage: $0 <seedData> <orderProcessorServer>}"
SERVER="${2:?usage: $0 <seedData> <orderProcessorServer>}"
WORK="$(mktemp -d)"
trap 'rm -rf -- "${WORK:?}"' EXIT
export EXCHANGE_LOG="$WORK/exchange.log"

failures=0
check() {
    if [[ "$2" == "$3" ]]; then
        echo "ok    $1"
    else
        echo "FAIL  $1: got '$2', wanted '$3'"
        failures=$((failures + 1))
    fi
}

# 1. A server runs on a new directory, unseeded, until it listens, and stops
port="$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')"
"$SERVER" --port "$port" --data-dir "$WORK/unseeded" > "$WORK/server.out" 2>&1 &
server=$!
for _ in $(seq 300); do
    (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && break
    sleep 0.2
done
kill -TERM "$server"
wait "$server"
check "an unseeded server ran and stopped cleanly" "$?" 0
check "it left an LMDB file" "$([[ -e "$WORK/unseeded/data.mdb" ]] && echo yes || echo no)" yes

# 2. --if-empty seeds that directory: it has no instrument
out="$("$SEED" --if-empty --data-dir "$WORK/unseeded" 2>&1)"
check "--if-empty seeds a directory a server ran on unseeded" "$(grep -c '^Seeded 10 instruments' <<<"$out")" 1

# 3. ... and leaves it alone once it has instruments
out="$("$SEED" --if-empty --data-dir "$WORK/unseeded" 2>&1)"
status=$?
check "--if-empty on a directory with instruments: exit status" "$status" 0
check "--if-empty leaves a directory with instruments alone" "$(grep -c "isn't seeded (--if-empty)" <<<"$out")" 1
check "... and adds nothing" "$(grep -c '^  Added' <<<"$out")" 0

# 4. A new directory is seeded
out="$("$SEED" --if-empty --data-dir "$WORK/new" 2>&1)"
check "--if-empty seeds a new directory" "$(grep -c '^Seeded 10 instruments' <<<"$out")" 1

(( failures == 0 )) || exit 1
