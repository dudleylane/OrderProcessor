#!/usr/bin/env bash
# PgMigrateTest.sh <pg-migrate.sh>: the migration script's decisions (#590), against made-up migrations and a stand-in
# psql that keeps the volume's version in a file: the tree's own migrations pass --check; a DROP, a gap, or init-db at
# another version are refused before anything is applied; a volume is brought up in order and its version read after
# each; a second run applies nothing; a volume newer than every migration is refused; and a migration that fails, or
# that doesn't record its version, stops the run with what comes after it unapplied.
set -uo pipefail
SCRIPT=${1:?pg-migrate.sh}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
failures=0
check() { # <name> <expected exit> <command...>
    local name=$1 want=$2
    shift 2
    "$@" > "$WORK/out" 2>&1
    local got=$?
    if [[ "$got" != "$want" ]]; then
        echo "FAIL $name: exit $got, want $want"
        sed 's/^/  /' "$WORK/out"
        failures=$((failures + 1))
    else
        echo "ok   $name"
    fi
}

# The stand-in psql, which answers as Postgres would: whether schema_version exists (not at version 0, a volume made
# before versioning), and its version, which a query naming the table fails to read when there is none; a migration,
# given as -f - in one transaction, records the version it inserts, or fails on FAIL
cat > "$WORK/psql" << 'STUB'
#!/usr/bin/env bash
state=${STUB_STATE:?}
args=" $* "
if [[ "$args" == *" -c "* ]]; then
    if [[ "$args" == *"to_regclass('schema_version')"* ]]; then
        [[ "$(cat "$state")" == 0 ]] && echo f || echo t
        exit 0
    fi
    if [[ "$args" == *"FROM schema_version"* && "$(cat "$state")" == 0 ]]; then
        echo 'ERROR:  relation "schema_version" does not exist' >&2
        exit 1
    fi
    cat "$state"
    exit 0
fi
if [[ "$args" != *" --single-transaction "* || "$args" != *" -f - "* ]]; then
    echo "a migration must come as -f - in one transaction: $*" >&2
    exit 4
fi
sql=$(cat)
if [[ "$sql" == *FAIL* ]]; then exit 3; fi
echo "applied $(printf '%s' "$sql" | head -n 1)" >> "$state.log"
if [[ "$sql" =~ VALUES\ \(([0-9]+)\) ]]; then echo "${BASH_REMATCH[1]}" > "$state"; fi
STUB
chmod +x "$WORK/psql"
export PSQL="$WORK/psql" STUB_STATE="$WORK/state"

setup() { # <dir> <init version> <migration text>...: migrations 001 up, each the text given
    local dir=$1 init=$2 n=0
    shift 2
    rm -rf "$dir"
    mkdir -p "$dir"
    echo "INSERT INTO schema_version (version) VALUES ($init);" > "$dir/init.sql"
    for text in "$@"; do
        n=$((n + 1))
        printf '%s\n' "$text" > "$dir/$(printf '%03d' "$n")_m.sql"
    done
}
run() { # <dir> [--check]
    PG_MIGRATIONS_DIR="$1" PG_INIT_SCHEMA="$1/init.sql" bash "$SCRIPT" "${@:2}"
}

check "the tree's migrations pass --check" 0 bash "$SCRIPT" --check

setup "$WORK/drop" 1 "-- m1
DROP TABLE orders; INSERT INTO schema_version (version) VALUES (1);"
check "a migration holding DROP is refused" 2 run "$WORK/drop" --check
setup "$WORK/gap" 2 "-- m1
INSERT INTO schema_version (version) VALUES (1);" "-- m2
INSERT INTO schema_version (version) VALUES (2);"
mv "$WORK/gap/002_m.sql" "$WORK/gap/003_m.sql"
check "a gap in the numbers is refused" 2 run "$WORK/gap" --check
setup "$WORK/init" 3 "-- m1
INSERT INTO schema_version (version) VALUES (1);"
check "init-db at another version than the last migration is refused" 2 run "$WORK/init" --check

setup "$WORK/up" 2 "-- m1
INSERT INTO schema_version (version) VALUES (1);" "-- m2
INSERT INTO schema_version (version) VALUES (2);"
echo 0 > "$WORK/state"
rm -f "$WORK/state.log"
check "a volume at 0 is brought to 2" 0 run "$WORK/up"
if [[ "$(cat "$WORK/state")" != 2 || "$(cat "$WORK/state.log")" != $'applied -- m1\napplied -- m2' ]]; then
    echo "FAIL the migrations went in order: state $(cat "$WORK/state"), log $(tr '\n' '|' < "$WORK/state.log")"
    failures=$((failures + 1))
fi
check "a second run applies nothing" 0 run "$WORK/up"
[[ "$(wc -l < "$WORK/state.log")" == 2 ]] || { echo "FAIL the second run applied something"; failures=$((failures + 1)); }
echo 1 > "$WORK/state"
rm -f "$WORK/state.log"
check "a volume at 1 gets 2 alone" 0 run "$WORK/up"
[[ "$(cat "$WORK/state.log")" == "applied -- m2" ]] || { echo "FAIL more than 2 applied"; failures=$((failures + 1)); }
echo 5 > "$WORK/state"
check "a volume newer than every migration is refused" 2 run "$WORK/up"

setup "$WORK/fails" 2 "-- m1
FAIL" "-- m2
INSERT INTO schema_version (version) VALUES (2);"
echo 0 > "$WORK/state"
rm -f "$WORK/state.log"
check "a failing migration stops the run" 1 run "$WORK/fails"
[[ ! -e "$WORK/state.log" && "$(cat "$WORK/state")" == 0 ]] || { echo "FAIL a migration after the failing one ran"; failures=$((failures + 1)); }
setup "$WORK/unrecorded" 1 "-- m1
SELECT 1;"
echo 0 > "$WORK/state"
check "a migration that records no version stops the run" 1 run "$WORK/unrecorded"

if ((failures == 0)); then
    echo "pg-migrate: every case passed"
else
    echo "pg-migrate: $failures case(s) failed"
    exit 1
fi
