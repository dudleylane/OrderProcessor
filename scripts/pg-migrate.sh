#!/usr/bin/env bash
# pg-migrate.sh [--check]: brings a Postgres volume's schema to the version the server's statements are written for
# (#590), applying each migration in docker/migrations above the volume's version, in order, each in one transaction.
# The server never runs DDL: an operator runs this, as the database's owner, with the stack up. A migration may hold no
# DROP, so nothing a volume holds is lost. A volume made before the schema was versioned is at version 0.
#   PSQL               the psql command, connected as the owner; default compose's, run from docker/:
#                      docker compose exec -T oms-db psql -U oms -d oms
#   PG_MIGRATIONS_DIR  where the migrations are (docker/migrations); PG_INIT_SCHEMA, init-db's schema (a test's own)
#   --check            checks the migrations alone, connecting to nothing: numbered NNN_*.sql from 001 with no gap,
#                      none holding DROP, and the last one the version init-db/001_schema.sql makes a new volume at
# Exit 0 once the volume is at the last migration's version (or --check passes), 1 when a migration failed or didn't
# record its version, 2 for a usage error, a migration refused, or a volume newer than every migration.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${PG_MIGRATIONS_DIR:-$ROOT/docker/migrations}"
INIT="${PG_INIT_SCHEMA:-$ROOT/docker/init-db/001_schema.sql}"
refuse() {
    echo "pg-migrate: $*" >&2
    exit 2
}
check=0
case "${1:-}" in
    --check) check=1 ;;
    "") ;;
    *) refuse "unknown argument $1; usage: pg-migrate.sh [--check]" ;;
esac

# The migrations, checked whole before anything is applied
files=()
while IFS= read -r f; do files+=("$f"); done < <(find "$DIR" -maxdepth 1 -name '[0-9][0-9][0-9]_*.sql' | sort)
((${#files[@]} > 0)) || refuse "no migrations in $DIR"
expected=1
for f in "${files[@]}"; do
    n=$((10#$(basename "$f" | cut -c1-3)))
    ((n == expected)) || refuse "$(basename "$f"): expected migration $(printf '%03d' "$expected"), as they're numbered from 001 with no gap"
    if grep -qiwE 'drop' "$f"; then
        refuse "$(basename "$f") holds DROP: a migration may drop nothing a volume holds (#590)"
    fi
    expected=$((n + 1))
done
last=$((expected - 1))
made=$(grep -oE 'INSERT INTO schema_version \(version\) VALUES \([0-9]+\)' "$INIT" | grep -oE '[0-9]+' | tail -n 1)
[[ "$made" == "$last" ]] || refuse "init-db makes a new volume at version ${made:-none}, and the last migration is $last"
if ((check)); then
    echo "pg-migrate: ${#files[@]} migration(s), up to version $last, as init-db makes a new volume"
    exit 0
fi

read -ra psql <<< "${PSQL:-docker compose exec -T oms-db psql -U oms -d oms}"
query() { # <sql>: one value, as psql prints it unaligned
    (cd "$ROOT/docker" && "${psql[@]}" -Atq -v ON_ERROR_STOP=1 -c "$1")
}
version_of() { # in two queries, as the server's writer reads it: Postgres resolves every table a statement names as it
    # parses it, whatever a CASE would take, so one query naming schema_version fails on a volume without it
    local exists
    exists=$(query "SELECT to_regclass('schema_version') IS NOT NULL") || return 1
    if [[ "$exists" != "t" ]]; then
        echo 0
        return 0
    fi
    query "SELECT coalesce(max(version), 0) FROM schema_version"
}
version=$(version_of) || { echo "pg-migrate: can't read the volume's version" >&2; exit 1; }
[[ "$version" =~ ^[0-9]+$ ]] || { echo "pg-migrate: the volume's version reads as '$version'" >&2; exit 1; }
((version <= last)) || refuse "the volume is at version $version, newer than these migrations ($last): this checkout is older than the volume"
if ((version == last)); then
    echo "pg-migrate: the volume is at version $version; nothing to apply"
    exit 0
fi
for f in "${files[@]:$version}"; do
    n=$((10#$(basename "$f" | cut -c1-3)))
    echo "pg-migrate: applying $(basename "$f")"
    # -f -: psql wraps only what -c or -f gives it in --single-transaction, and reads the file from stdin
    (cd "$ROOT/docker" && "${psql[@]}" -q -v ON_ERROR_STOP=1 --single-transaction -f - < "$f") \
        || { echo "pg-migrate: $(basename "$f") failed, and its transaction was rolled back" >&2; exit 1; }
    now=$(version_of) || { echo "pg-migrate: can't read the volume's version after $(basename "$f")" >&2; exit 1; }
    [[ "$now" == "$n" ]] || { echo "pg-migrate: $(basename "$f") left the volume at version '$now', not $n" >&2; exit 1; }
done
echo "pg-migrate: the volume is at version $last"
