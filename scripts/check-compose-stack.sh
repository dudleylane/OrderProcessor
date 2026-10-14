#!/usr/bin/env bash
# check-compose-stack.sh: bring up docker/docker-compose.yml's stack and check that it works (#140).
#
# The image checks, check-server-image.sh and check-frontend-image.sh, run each image
# alone. This runs what docker/README.md's quick start runs, compose up --build, so it
# covers what only compose exercises: docker-compose.yml's services, health checks,
# dependencies, volumes and entrypoint, docker/.env.example, the schema in
# docker/init-db, and nginx passing /ws to the server by its service name. It runs
# podman-compose under a project name of its own, with an override file that publishes
# no host port (this machine runs PostgreSQL on 5432, and development servers use 3000
# and 8080) and gives oms-db .env.example's settings, with an access token made for the
# run and an Origin list, so it neither reads nor writes a docker/.env. On this branch it
# also mounts a FIX credentials file made for the run, as docker/README.md's quick start
# makes one (#147), in place of a docker/fix-credentials it never reads or writes. Then:
#   0. Before it builds anything: compose refuses .env.example as it is, which sets no
#      access token, and docker-compose.yml publishes only the frontend's port (#146)
#      and, on this branch, the FIX acceptor's 5001.
#   1. The services become healthy, in their depends_on order: oms-db, oms-server,
#      oms-frontend, within START_TIMEOUT. Their health checks are compose's, since
#      podman-compose builds OCI images, which keep none of their own. On an SELinux
#      host, docker/init-db first gets the label a fresh clone has: compose's :z mount
#      relabels it for good, and a relabelled copy would hide a mount without :z (#201).
#   2. Postgres created every table docker/init-db/*.sql creates.
#   3. oms-server seeded its new volume before it started (#161).
#   4. Through nginx, inside the frontend's container: GET / is the app, and /ws answers
#      a WebSocket upgrade that offers the token with the server's 101 and its
#      subprotocol, one without the token with 401, and one from a page off the Origin
#      list with 403 (#146).
#   5. A client on the stack's network sends a new order to the frontend's /ws, offering
#      the token as the app does, and gets an execution report with status NEW: a
#      browser's order's path, through nginx, to a seeded engine.
#   6. Stopping the stack ends every container with exit code 0, and the server's log
#      has no errors.
# The containers, pod, network, volumes and images it makes are removed when it exits,
# pass or fail. The base images stay, so the next build only checks them for an update.
#
# CI runs this when what it covers changes, and weekly (.github/workflows/compose.yml).
# Run it the same way before changing anything compose runs:
#   scripts/check-compose-stack.sh
# It needs podman, podman-compose and network access (quay.io, docker.io, dnf mirrors,
# github.com and the npm registry), and takes about five minutes, because both images
# are built from scratch.
#
# Environment:
#   CHECK_NAME     the compose project's name, which names everything it makes
#                  (default: orderprocessor-check-<pid>)
#   START_TIMEOUT  seconds compose up, after the build, may take to start the stack and
#                  the services to become healthy (default: 300)
#
# Exit codes: 0 = the stack came up and passed every check, 1 = it didn't,
# 2 = usage or setup error

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${CHECK_NAME:-orderprocessor-check-$$}"
START_TIMEOUT="${START_TIMEOUT:-300}"
SERVICES=(oms-db oms-server oms-frontend) # in their depends_on order
FRONTEND_PORT=3000                        # the port docker/nginx.conf listens on
# What docker-compose.yml may publish on the host, as "service published:container" (#146)
PUBLISHED=("oms-frontend 3000:3000" "oms-server 5001:5001")

usage_error() {
    echo "check-compose-stack: $1" >&2
    exit 2
}

case "${1:-}" in
    "") ;;
    -h|--help)
        sed -n '2,/^$/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
        exit 0 ;;
    *) usage_error "unknown argument '$1' (usage: $0 [--help])" ;;
esac
[[ "$NAME" =~ ^[a-z0-9][a-z0-9_.-]*$ ]] || usage_error "CHECK_NAME '$NAME' isn't a valid project name"
[[ "$START_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || usage_error "START_TIMEOUT '$START_TIMEOUT' isn't a positive number"
command -v podman >/dev/null 2>&1 || usage_error "podman is not installed"
command -v podman-compose >/dev/null 2>&1 || usage_error "podman-compose is not installed"
WORK="$(mktemp -d)"

step() {
    echo
    echo "=== check-compose-stack: $1 ==="
}

fail() {
    echo >&2
    echo "check-compose-stack: FAILED: $1" >&2
    exit 1
}

# podman-compose on the committed file plus this run's override. --layers=false and --pull=newer as in the image checks:
# no intermediate images left in the store this user shares, and current base images.
COMPOSE=(podman-compose "--podman-build-args=--layers=false --pull=newer" -p "$NAME"
    -f docker/docker-compose.yml -f "$WORK/override.yml" --env-file "$WORK/env")

# Prints the name of the container compose made for service $1
container() {
    podman ps -a --filter "label=io.podman.compose.project=$NAME" --filter "label=com.docker.compose.service=$1" \
        --format '{{.Names}}' | head -n 1
}

# On any exit: show what the services said if a check failed, then remove what this run made.
cleanup() {
    local status=$? service c
    if (( status != 0 )); then
        for service in "${SERVICES[@]}"; do
            c="$(container "$service" 2>/dev/null)" || true
            [[ -n "$c" ]] || continue
            step "$service's output"
            podman logs "$c" 2>&1 | tail -n 30 || true
        done
    fi
    if [[ -f "$WORK/override.yml" ]]; then
        "${COMPOSE[@]}" down -v -t 0 >/dev/null 2>&1 || true
    fi
    # Whatever compose down didn't reach, such as what a failed up left
    podman ps -a -q --filter "label=io.podman.compose.project=$NAME" | xargs -r podman rm -f -t 0 \
        >/dev/null 2>&1 || true
    podman pod rm -f "pod_$NAME" >/dev/null 2>&1 || true
    podman network rm -f "${NAME}_default" >/dev/null 2>&1 || true
    podman volume ls -q --filter "label=io.podman.compose.project=$NAME" | xargs -r podman volume rm -f \
        >/dev/null 2>&1 || true
    podman rmi -f "localhost/${NAME}_oms-server" "localhost/${NAME}_oms-frontend" >/dev/null 2>&1 || true
    rm -rf "${WORK:?}"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

cd "$ROOT"

# compose takes a variable from the shell before its env file (podman-compose 1.5.0), so a setting exported here, such
# as a developer's OMS_WS_TOKEN, would replace this run's own, and fail the checks that use it
while IFS='=' read -r variable _; do
    unset "$variable"
done < <(grep -E '^[A-Za-z_][A-Za-z0-9_]*=' docker/.env.example | tr -d '\r')

# .env.example's settings, for compose's variables and for oms-db's environment
cp docker/.env.example "$WORK/env"
setting() {
    sed -n "s/^$1=//p" "$WORK/env" | tr -d '\r' | tail -n 1
}
DB_USER="$(setting POSTGRES_USER)"
DB_NAME="$(setting POSTGRES_DB)"
[[ -n "$DB_USER" && -n "$DB_NAME" ]] || fail "docker/.env.example sets no POSTGRES_USER or POSTGRES_DB"
# This run's access token, and the one Origin a browser page may connect from (#146)
grep -q '^OMS_WS_TOKEN=' "$WORK/env" || fail "docker/.env.example has no OMS_WS_TOKEN line"
grep -q '^OMS_ALLOWED_ORIGINS=' "$WORK/env" || fail "docker/.env.example has no OMS_ALLOWED_ORIGINS line"
TOKEN="$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')"
ORIGIN="http://localhost:$FRONTEND_PORT"
sed -i -e "s/^OMS_WS_TOKEN=.*/OMS_WS_TOKEN=$TOKEN/" -e "s|^OMS_ALLOWED_ORIGINS=.*|OMS_ALLOWED_ORIGINS=$ORIGIN|" "$WORK/env"
# Resets only the ports PUBLISHED names, which the step below checks are all there are: podman-compose 1.5.0 fails on a
# !reset of a key the file doesn't have
cat > "$WORK/override.yml" <<EOF
# check-compose-stack.sh's changes to docker/docker-compose.yml: no host ports, and .env.example for oms-db
services:
  oms-db:
    env_file: !override
      - $WORK/env
  oms-server:
    ports: !reset []
    volumes: !override
      - oms-data:/data
      - $WORK/fix-credentials:/etc/orderprocessor/fix-credentials:ro,z
  oms-frontend:
    ports: !reset []
EOF
# The FIX credentials compose's server needs (#147), as docker/README.md's quick start makes them: a counterparty
# trading a seeded account, with a PBKDF2 hash. Only the hash is in the file, so it may be readable by others.
# shellcheck disable=SC2016 # the $ are the Python f-string's, inside the single quotes on purpose
python3 -c 'import hashlib, os; s = os.urandom(16); k = hashlib.pbkdf2_hmac("sha256", b"check-password", s, 600000); '\
'print(f"CHECK_CLIENT check pbkdf2-sha256$600000${s.hex()}${k.hex()} TRADING-1")' > "$WORK/fix-credentials" \
    || fail "couldn't make the FIX credentials file"
chmod 644 "$WORK/fix-credentials"

step "what compose would publish, and whether it starts without a token"
# docker-compose.yml as committed: this run's override publishes nothing, so the stack's ports are checked here
published="$(podman-compose -p "$NAME" -f docker/docker-compose.yml --env-file "$WORK/env" config 2>/dev/null | awk '
    /^  [^ ].*:$/ { service = $1; sub(/:$/, "", service); inports = 0; next }
    /^    ports:$/ { inports = 1; next }
    /^    [^ -]/ { inports = 0 }
    inports && /^    - / { sub(/^    - /, ""); print service " " $0 }')" || fail "compose config failed"
[[ "$published" == "$(printf '%s\n' "${PUBLISHED[@]}")" ]] \
    || fail "docker-compose.yml publishes '${published//$'\n'/, }', not only '${PUBLISHED[*]}'"
echo "published: ${published//$'\n'/, }"
if podman-compose -p "$NAME" -f docker/docker-compose.yml --env-file docker/.env.example config >/dev/null 2>&1; then
    fail "compose accepts docker/.env.example, which sets no OMS_WS_TOKEN: its server would let anyone in"
fi
echo "compose refuses docker/.env.example as it is: it sets no OMS_WS_TOKEN"

if command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled; then
    chcon -R --reference=docker docker/init-db || fail "couldn't give docker/init-db its directory's SELinux label"
fi

step "build the images, as compose up --build does ($(podman-compose --version 2>/dev/null | tail -n 1))"
"${COMPOSE[@]}" build || fail "compose build failed"
build_seconds=$SECONDS

step "compose up, as project $NAME"
# up waits for each depends_on condition, service_healthy here, before it starts the next service, with no limit of its
# own: a service that never becomes healthy would hold up until the job's timeout.
up=0
timeout -k 10 "$START_TIMEOUT" "${COMPOSE[@]}" up -d || up=$?
if (( up != 0 )); then
    for service in "${SERVICES[@]}"; do
        c="$(container "$service")"
        if [[ -n "$c" ]]; then
            echo "$service: $(podman inspect --format '{{.State.Status}}, health {{.State.Health.Status}}' "$c")"
        else
            echo "$service: no container"
        fi
    done
    (( up != 124 )) || fail "compose up didn't start the stack within ${START_TIMEOUT}s"
    fail "compose up failed, with exit code $up"
fi

step "the services' health"
# up waited for oms-db and oms-server, which other services depend on, but not for oms-frontend
deadline=$((SECONDS + START_TIMEOUT))
for service in "${SERVICES[@]}"; do
    health=""
    while :; do
        c="$(container "$service")"
        if [[ -n "$c" ]]; then
            health="$(podman inspect --format '{{.State.Health.Status}}' "$c")"
            [[ "$health" == healthy ]] && break
            [[ "$(podman inspect --format '{{.State.Status}}' "$c")" != exited ]] \
                || fail "$service exited, with code $(podman inspect --format '{{.State.ExitCode}}' "$c")"
        fi
        (( SECONDS < deadline )) || fail "$service isn't healthy after ${START_TIMEOUT}s (${health:-no container})"
        sleep 2
    done
    echo "$service ($c): healthy"
done
db="$(container oms-db)"
server="$(container oms-server)"
frontend="$(container oms-frontend)"

step "the schema"
mapfile -t want < <(sed -n 's/^CREATE TABLE \([a-z_]*\).*/\1/p' docker/init-db/*.sql | sort -u)
(( ${#want[@]} > 0 )) || fail "found no CREATE TABLE in docker/init-db"
have="$(podman exec "$db" psql -U "$DB_USER" -d "$DB_NAME" -Atc \
    "SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1")" || fail "psql failed"
missing=()
for table in "${want[@]}"; do
    grep -qx "$table" <<< "$have" || missing+=("$table")
done
(( ${#missing[@]} == 0 )) || fail "the database has no table ${missing[*]}"
echo "all ${#want[@]} tables docker/init-db creates: ${want[*]}"

step "the server's data"
seeded="$(podman logs "$server" 2>&1 | grep -m 1 '^Seeded ' || true)"
[[ -n "$seeded" ]] || fail "oms-server didn't seed its new volume: its output has no 'Seeded' line"
echo "$seeded"

step "the server's ports"
# Every port its image EXPOSEs listens in the stack, as check-server-image.sh checks the image alone. On this branch that
# is also 5001, the FIX acceptor's, which compose's command starts with --fix-cfg: a merge dropped that flag once (#161),
# and nothing else here would notice.
mapfile -t ports < <(podman image inspect --format '{{range $p, $_ := .Config.ExposedPorts}}{{println $p}}{{end}}' \
    "$(podman inspect --format '{{.Image}}' "$server")" | sed '/^$/d')
(( ${#ports[@]} > 0 )) || fail "oms-server's image exposes no port"
deadline=$((SECONDS + START_TIMEOUT))
while :; do
    waiting=()
    for port in "${ports[@]}"; do
        podman exec "$server" timeout 2 bash -c "exec 3<>/dev/tcp/127.0.0.1/${port%/tcp}" 2>/dev/null \
            || waiting+=("${port%/tcp}")
    done
    (( ${#waiting[@]} == 0 )) && break
    (( SECONDS < deadline )) || fail "after ${START_TIMEOUT}s nothing in oms-server listens on ${waiting[*]}"
    sleep 1
done
echo "oms-server listens on ${ports[*]%/tcp}"

step "through nginx"
page="$(podman exec "$frontend" curl -s --max-time 5 -w '\n%{http_code}' "http://127.0.0.1:$FRONTEND_PORT/")" || true
[[ "${page##*$'\n'}" == 200 ]] || fail "GET / answered '${page##*$'\n'}', not 200"
grep -q '<div id="root"></div>' <<< "$page" || fail "GET / isn't the app's index.html: it has no <div id=\"root\">"
echo "GET / is the app's index.html"
# Prints the answer's head to a WebSocket upgrade on /ws with the headers given. An upgraded connection stays open, so
# curl ends at --max-time; the head is what counts.
upgrade() {
    podman exec "$frontend" curl -s -i -N --max-time 3 -H 'Connection: Upgrade' -H 'Upgrade: websocket' \
        -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' "$@" \
        "http://127.0.0.1:$FRONTEND_PORT/ws" | tr -d '\r' | sed '/^$/q' || true
}
OFFER="Sec-WebSocket-Protocol: oms-v1, oms-token.$TOKEN"
answer="$(upgrade -H "$OFFER" -H "Origin: $ORIGIN")"
[[ "$answer" == "HTTP/1.1 101"* ]] || fail "/ws answered '${answer%%$'\n'*}' to an upgrade with the token, not 101"
grep -qix 'sec-websocket-protocol: oms-v1' <<< "$answer" || fail "the 101 doesn't name the subprotocol oms-v1: $answer"
echo "/ws, with the token, from $ORIGIN: ${answer%%$'\n'*}, subprotocol oms-v1"
answer="$(upgrade -H 'Sec-WebSocket-Protocol: oms-v1')"
[[ "$answer" == "HTTP/1.1 401"* ]] || fail "/ws answered '${answer%%$'\n'*}' to an upgrade without the token, not 401"
echo "/ws, without the token: ${answer%%$'\n'*}"
answer="$(upgrade -H "$OFFER" -H 'Origin: http://elsewhere.example')"
[[ "$answer" == "HTTP/1.1 403"* ]] || fail "/ws answered '${answer%%$'\n'*}' to a page off the Origin list, not 403"
echo "/ws, with the token, from another Origin: ${answer%%$'\n'*}"

step "a new order through nginx"
# A client on the stack's network, in the frontend's builder image, which the build has just pulled
node_image="$(sed -n 's/^FROM \(node:[^ ]*\).*/\1/p' docker/Dockerfile.oms-frontend | head -n 1 | tr -d '\r')"
[[ -n "$node_image" ]] || fail "docker/Dockerfile.oms-frontend has no node builder image to run the client in"
network="$(podman inspect --format '{{range $name, $_ := .NetworkSettings.Networks}}{{$name}} {{end}}' "$frontend")"
network="${network%% *}"
client='
const ws = new WebSocket(process.argv[1], ["oms-v1", "oms-token." + process.argv[2]]);
const timer = setTimeout(() => { console.log("no execution report within 10 s"); process.exit(1); }, 10000);
ws.onerror = () => { console.log("no connection to " + process.argv[1]); process.exit(1); };
ws.onopen = () => {
    if (ws.protocol !== "oms-v1") { console.log("the server answered with subprotocol \"" + ws.protocol + "\""); process.exit(1); }
    ws.send(JSON.stringify({ type: "new_order", data: { symbol: "AAPL", side: "BUY", ordType: "LIMIT", price: 150.25,
        orderQty: 100, tif: "DAY", currency: "USD", capacity: "AGENCY" } }));
};
ws.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.type === "error") { console.log("error: " + message.message); process.exit(1); }
    if (message.type === "execution_report") {
        console.log("execution_report " + message.data.orderStatus);
        clearTimeout(timer);
        ws.close();
        process.exit(0);
    }
};'
report="$(podman run --rm --network "$network" "$node_image" node -e "$client" \
    "ws://oms-frontend:$FRONTEND_PORT/ws" "$TOKEN" 2>&1)" || fail "the order got no execution report: $report"
[[ "$report" == "execution_report NEW" ]] || fail "the order got '$report', not an execution report with status NEW"
echo "$report"

step "stop the stack"
"${COMPOSE[@]}" stop >/dev/null 2>&1 || fail "compose stop failed"
for service in "${SERVICES[@]}"; do
    c="$(container "$service")"
    code="$(podman inspect --format '{{.State.ExitCode}}' "$c")"
    [[ "$code" == 0 ]] || fail "$service exited with code $code"
done
echo "every container exited with code 0"
podman logs "$server" > "$WORK/server.log" 2>&1 || fail "podman logs failed"
errors="$(grep -c -E ': \[(Error|Fatal)\]\[' "$WORK/server.log" || true)"
if (( errors > 0 )); then
    grep -E ': \[(Error|Fatal)\]\[' "$WORK/server.log" >&2
    fail "the server logged $errors error(s)"
fi
echo "the server's log: $(wc -l < "$WORK/server.log") lines, no errors"

step "passed in ${SECONDS}s (the build took ${build_seconds}s)"
