#!/usr/bin/env bash
# pg-for-tests.sh: a throwaway PostgreSQL, in a container, with this repository's schema, for the Postgres write-behind's
# tests (#151). The host's own PostgreSQL, its roles and its secrets are left alone.
#
# usage: scripts/pg-for-tests.sh start NAME   start it, and print its connection string on standard output
#        scripts/pg-for-tests.sh stop NAME    remove it
#
# start runs the image docker/docker-compose.yml gives oms-db, as container NAME, published on 127.0.0.1 at a port the
# system picks, with a password made for the run. Once the server takes TCP connections, it applies docker/init-db/*.sql
# in order, stopping at the first error, and prints a libpq connection string for PG_CONNECTION_STRING:
#   PG_CONNECTION_STRING="$(scripts/pg-for-tests.sh start pg-test)" ctest -R PGWriteBehind
#   scripts/pg-for-tests.sh stop pg-test
# CI runs it so (.github/workflows/build-test.yml and sanitizers.yml). It needs podman.
#
# Exit codes: 0 = done, 1 = the database didn't start or the schema didn't apply, 2 = usage error

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
usage() {
    echo "usage: $0 start|stop NAME" >&2
    exit 2
}
(( $# == 2 )) || usage
ACTION=$1
NAME=$2
[[ "$NAME" =~ ^[a-z0-9][a-z0-9_.-]*$ ]] || usage
command -v podman >/dev/null 2>&1 || { echo "pg-for-tests: podman is not installed" >&2; exit 2; }

case "$ACTION" in
    stop)
        podman rm -f -t 0 "$NAME" >/dev/null 2>&1 || true
        exit 0 ;;
    start) ;;
    *) usage ;;
esac

# The image compose runs, so the tests and the stack use one PostgreSQL
IMAGE="$(sed -n '/^  oms-db:/,/^  [a-z]/s/^    image: *//p' "$ROOT/docker/docker-compose.yml" | tr -d '\r' | head -n 1)"
[[ -n "$IMAGE" ]] || { echo "pg-for-tests: docker/docker-compose.yml gives oms-db no image" >&2; exit 1; }
PASSWORD="$(head -c 24 /dev/urandom | base64 | tr -dc 'A-Za-z0-9')"
fail() {
    echo "pg-for-tests: $1" >&2
    podman logs "$NAME" 2>&1 | tail -n 20 >&2 || true
    podman rm -f -t 0 "$NAME" >/dev/null 2>&1 || true
    exit 1
}

podman rm -f -t 0 "$NAME" >/dev/null 2>&1 || true
podman run -d --name "$NAME" -e POSTGRES_USER=oms -e POSTGRES_DB=oms -e POSTGRES_PASSWORD="$PASSWORD" \
    -p 127.0.0.1::5432 "$IMAGE" >/dev/null || fail "the container didn't start"
PORT="$(podman port "$NAME" 5432/tcp | head -n 1)"
PORT="${PORT##*:}"
[[ "$PORT" =~ ^[0-9]+$ ]] || fail "no published port for 5432"

# Over TCP: the server the image runs while it initialises listens on its socket only, then stops
for _ in $(seq 1 120); do
    podman exec "$NAME" pg_isready -q -h 127.0.0.1 -U oms -d oms 2>/dev/null && break
    sleep 0.5
done
podman exec "$NAME" pg_isready -q -h 127.0.0.1 -U oms -d oms 2>/dev/null || fail "PostgreSQL didn't take connections in 60s"

for sql in "$ROOT"/docker/init-db/*.sql; do
    podman exec -i "$NAME" psql -q -v ON_ERROR_STOP=1 -h 127.0.0.1 -U oms -d oms < "$sql" >/dev/null \
        || fail "$(basename "$sql") didn't apply"
done
echo "host=127.0.0.1 port=$PORT dbname=oms user=oms password=$PASSWORD"
