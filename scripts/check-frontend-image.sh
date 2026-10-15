#!/usr/bin/env bash
# check-frontend-image.sh: build the frontend's Docker image and check that it works (#139).
#
# Nothing else builds the frontend. CI and the pre-push hook build the C++ side only, so
# a TypeScript error in src/oms-frontend turned no check red, a wire-protocol type that
# app/JsonSerializer.cpp no longer matches among them. The image's build runs `npm ci`
# and `npm run build` (`tsc && vite build`), so building it is the check that the
# frontend compiles. This builds it as `docker compose up --build` does: from
# docker/Dockerfile.oms-frontend, with the repository as context, and in docker format,
# which keeps its HEALTHCHECK. Then, on a podman network of its own:
#   1. The image runs as nginx, not root, and EXPOSEs 3000, the port nginx.conf listens
#      on. (It also EXPOSEs 80, nginx:alpine's, where nothing listens.)
#   2. Without a certificate it doesn't start, and says how to make one (#226).
#   3. With a throwaway certificate mounted, its key readable only by its owner and
#      group 0 as docker/README.md says, it starts while nothing on the network is
#      called oms-server, and listens: nginx looks the server up per request, not once
#      at startup (#158). This and everything below is checked inside the container,
#      because rootless port forwarding accepts a connection on the host whether or not
#      anything listens behind it.
#   4. Every process in the container runs as nginx.
#   5. GET / over https answers 200 with the app's index.html, and the script it loads
#      answers 200 as JavaScript, with the certificate verified against its own name.
#   6. TLS only (#226): plain HTTP is redirected to the same URL over https, and no
#      Strict-Transport-Security is sent, unless OMS_HSTS sets it, as a second container
#      checks.
#   7. The image's health check passes.
#   8. A stand-in for the server joins the network as oms-server: the image itself,
#      running nginx with a configuration that answers with the WebSocket headers it was
#      sent. /ws reaches it through the frontend, with the Upgrade and Connection headers
#      a WebSocket upgrade needs.
#   9. podman stop ends nginx with exit code 0, and its log has no errors.
# The image, containers and network it creates are removed when it exits, pass or fail.
# The base images stay, so the next build only checks them for an update.
#
# CI runs this on every push (.github/workflows/docker.yml). Run it the same way before
# changing the frontend, its Dockerfile or docker/nginx.conf:
#   scripts/check-frontend-image.sh
# It needs podman, openssl and network access (docker.io and the npm registry), and takes
# about a minute, because every run builds from scratch.
#
# Environment:
#   CHECK_NAME     names the image (localhost/$CHECK_NAME), the network, the container,
#                  the stand-in ($CHECK_NAME-server) and the containers that check the
#                  start without a certificate and OMS_HSTS ($CHECK_NAME-nocert, -hsts)
#                  (default: orderprocessor-check-<pid>)
#   START_TIMEOUT  seconds nginx may take to listen (default: 30)
#
# Exit codes: 0 = the image built and passed every check, 1 = it didn't,
# 2 = usage or setup error

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${CHECK_NAME:-orderprocessor-check-$$}"
IMAGE="localhost/$NAME"
STAND_IN="$NAME-server"
NO_CERT="$NAME-nocert"
HSTS="$NAME-hsts"
START_TIMEOUT="${START_TIMEOUT:-30}"
PORT=3000 # the port docker/nginx.conf listens on
CERTS=/etc/nginx/certs # where docker/nginx.conf reads its certificate and key
# Where docker/nginx.conf sends /ws, so where the stand-in answers
SERVER_HOST=oms-server
SERVER_PORT=8080

usage_error() {
    echo "check-frontend-image: $1" >&2
    exit 2
}

case "${1:-}" in
    "") ;;
    -h|--help)
        sed -n '2,/^$/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
        exit 0 ;;
    *) usage_error "unknown argument '$1' (usage: $0 [--help])" ;;
esac
[[ "$NAME" =~ ^[a-z0-9][a-z0-9_.-]*$ ]] || usage_error "CHECK_NAME '$NAME' isn't a valid image name"
[[ "$START_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || usage_error "START_TIMEOUT '$START_TIMEOUT' isn't a positive number"
command -v podman >/dev/null 2>&1 || usage_error "podman is not installed"
command -v openssl >/dev/null 2>&1 || usage_error "openssl is not installed"
WORK="$(mktemp -d)"

step() {
    echo
    echo "=== check-frontend-image: $1 ==="
}

fail() {
    echo >&2
    echo "check-frontend-image: FAILED: $1" >&2
    exit 1
}

# On any exit: show what nginx and the stand-in said if a check failed, then remove what this run made.
cleanup() {
    local status=$? container
    if (( status != 0 )); then
        for container in "$NAME" "$STAND_IN" "$HSTS"; do
            podman container exists "$container" 2>/dev/null || continue
            step "$container's output"
            podman logs "$container" 2>&1 | tail -n 50 || true
        done
    fi
    podman rm -f -t 0 "$NAME" >/dev/null 2>&1 || true
    podman rm -f -t 0 "$STAND_IN" >/dev/null 2>&1 || true
    podman rm -f -t 0 "$NO_CERT" "$HSTS" >/dev/null 2>&1 || true
    podman network rm -f "$NAME" >/dev/null 2>&1 || true
    podman rmi -f "$IMAGE" >/dev/null 2>&1 || true
    rm -rf "${WORK:?}"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# GETs https://127.0.0.1:$PORT$1 inside the frontend's container, verifying nginx's certificate against the mounted one,
# and passing any further arguments to curl. Saves the body in $WORK/body, and prints "<status> <content type>", with
# status 000 if nothing answered.
get() {
    local path="$1" out
    shift
    out="$(podman exec "$NAME" curl -s --max-time 5 --cacert "$CERTS/tls.crt" \
        -w '\n%{http_code} %{content_type}' "$@" "https://127.0.0.1:$PORT$path")" || true
    printf '%s\n' "${out%$'\n'*}" > "$WORK/body"
    printf '%s\n' "${out##*$'\n'}"
}

cd "$ROOT"

step "build $IMAGE from docker/Dockerfile.oms-frontend ($(podman --version))"
# The build runs npm ci and npm run build (tsc && vite build), so a TypeScript error fails it. --layers=false keeps no
# intermediate images, so a run leaves nothing behind in the store it shares with everything else this user runs, and
# every run installs from the lockfile again. --pull=newer refreshes the base images, and uses the local copies when the
# registry can't be reached. --format docker keeps the HEALTHCHECK, which OCI drops.
podman build --layers=false --pull=newer --format docker -f docker/Dockerfile.oms-frontend -t "$IMAGE" . \
    || fail "the image didn't build"
build_seconds=$SECONDS

user="$(podman image inspect --format '{{.Config.User}}' "$IMAGE")"
[[ "$user" == nginx ]] || fail "the image runs as '${user:-root}', not nginx"
ports="$(podman image inspect --format '{{range $p, $_ := .Config.ExposedPorts}}{{$p}} {{end}}' "$IMAGE")"
[[ " $ports" == *" $PORT/tcp "* ]] || fail "the image exposes '$ports', not $PORT/tcp, the port nginx serves the app on"

step "without a certificate, it doesn't start"
# The image serves TLS only (#226), and its entrypoint stops at the check docker/nginx-tls-check.sh makes
podman run -d --name "$NO_CERT" "$IMAGE" >/dev/null || fail "couldn't run the image without a certificate"
running=true
for _ in $(seq 1 "$START_TIMEOUT"); do
    running="$(podman inspect --format '{{.State.Running}}' "$NO_CERT")"
    [[ "$running" == false ]] && break
    sleep 1
done
[[ "$running" == false ]] || fail "the image still runs ${START_TIMEOUT}s after starting without a certificate"
code="$(podman inspect --format '{{.State.ExitCode}}' "$NO_CERT")"
[[ "$code" != 0 ]] || fail "the image ran without a certificate, and exited 0"
podman logs "$NO_CERT" > "$WORK/nocert.log" 2>&1 || fail "podman logs failed"
grep -q "can't read $CERTS/tls.crt, so it can't serve TLS" "$WORK/nocert.log" \
    || fail "without a certificate it exited $code, without saying why: $(tail -n 3 "$WORK/nocert.log")"
echo "exit code $code: $(grep -o "can't read $CERTS/tls.crt[^.]*" "$WORK/nocert.log")"

step "start nginx with a throwaway certificate while nothing is called $SERVER_HOST, and wait for it to listen on $PORT"
# Named for each way the checks reach it: 127.0.0.1 inside the container, and the names the stack's clients use. Its key
# is mode 0640, as docker/README.md says: the container's nginx reads it as a member of group 0.
mkdir "$WORK/certs"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,DNS:oms-frontend,IP:127.0.0.1" \
    -keyout "$WORK/certs/tls.key" -out "$WORK/certs/tls.crt" 2>/dev/null || fail "openssl couldn't make a certificate"
chmod 0640 "$WORK/certs/tls.key"
chmod 0644 "$WORK/certs/tls.crt"
podman network create "$NAME" >/dev/null || fail "couldn't create the network $NAME"
# --health-interval=disable: the health check runs only when this script runs it. z: the label the containers share.
podman run -d --name "$NAME" --network "$NAME" --health-interval=disable -v "$WORK/certs:$CERTS:ro,z" "$IMAGE" \
    >/dev/null || fail "the container didn't start"
deadline=$((SECONDS + START_TIMEOUT))
until podman exec "$NAME" curl -s -o /dev/null --max-time 2 --cacert "$CERTS/tls.crt" "https://127.0.0.1:$PORT/" \
    2>/dev/null; do
    [[ "$(podman inspect --format '{{.State.Running}}' "$NAME")" == true ]] \
        || fail "nginx exited at start, with code $(podman inspect --format '{{.State.ExitCode}}' "$NAME")"
    (( SECONDS < deadline )) || fail "after ${START_TIMEOUT}s nothing listens on $PORT"
    sleep 1
done
echo "listening on $PORT"
users="$(podman top "$NAME" user | tail -n +2 | tr -d ' ' | sort -u | paste -sd ' ')"
[[ "$users" == nginx ]] || fail "the container's processes run as '$users', not nginx only"
echo "every process runs as nginx"

step "the app, over https"
status="$(get /)"
[[ "$status" == "200 text/html"* ]] || fail "GET / answered '${status:-nothing}', not 200 text/html"
grep -q '<div id="root"></div>' "$WORK/body" || fail "GET / isn't the app's index.html: it has no <div id=\"root\">"
script="$(grep -o 'src="/assets/[^"]*\.js"' "$WORK/body" | head -n 1 | sed -e 's/^src="//' -e 's/"$//')" || true
[[ -n "$script" ]] || fail "the app's index.html loads no script from /assets"
status="$(get "$script")"
[[ "$status" == "200 "*javascript* ]] || fail "GET $script answered '${status:-nothing}', not 200 as JavaScript"
echo "/ is the app's index.html, and its script $script is served ($(wc -c < "$WORK/body") bytes)"

step "TLS only"
redirect="$(podman exec "$NAME" curl -s -o /dev/null --max-time 5 -w '%{http_code} %{redirect_url}' \
    "http://127.0.0.1:$PORT/some/path?x=1")" || true
[[ "$redirect" == "301 https://127.0.0.1:$PORT/some/path?x=1" ]] \
    || fail "plain HTTP got '${redirect:-nothing}', not a 301 to https://127.0.0.1:$PORT/some/path?x=1"
echo "plain HTTP: $redirect"
hsts() {
    podman exec "$1" curl -s -I --max-time 5 --cacert "$CERTS/tls.crt" "https://127.0.0.1:$PORT/" \
        | tr -d '\r' | sed -n 's/^strict-transport-security: //Ip'
}
sent="$(hsts "$NAME")"
[[ -z "$sent" ]] || fail "without OMS_HSTS it sends Strict-Transport-Security: $sent"
echo "without OMS_HSTS: no Strict-Transport-Security"
podman run -d --name "$HSTS" -e OMS_HSTS="max-age=60" -v "$WORK/certs:$CERTS:ro,z" "$IMAGE" >/dev/null \
    || fail "the container with OMS_HSTS didn't start"
sent=""
for _ in $(seq 1 "$START_TIMEOUT"); do
    sent="$(hsts "$HSTS" 2>/dev/null)" || true
    [[ -n "$sent" ]] && break
    sleep 1
done
[[ "$sent" == "max-age=60" ]] || fail "with OMS_HSTS=max-age=60 it sends '${sent:-no Strict-Transport-Security}'"
echo "with OMS_HSTS=max-age=60: Strict-Transport-Security: $sent"
podman rm -f -t 0 "$HSTS" >/dev/null 2>&1 || true

step "the image's health check"
check="$(podman image inspect --format '{{json .HealthCheck}}' "$IMAGE")"
[[ -n "$check" && "$check" != null ]] || fail "the image has no health check"
echo "$check"
# Over https (#226): plain HTTP gets a redirect, which curl -f counts as a pass, so it would pass with TLS broken
[[ "$check" == *"https://127.0.0.1:$PORT/"* ]] || fail "the health check doesn't ask over https://127.0.0.1:$PORT/"
health=1
for _ in 1 2 3 4 5; do
    podman healthcheck run "$NAME" >/dev/null 2>&1 && { health=0; break; }
    sleep 1
done
(( health == 0 )) || fail "the health check fails while nginx serves the app"
echo "healthy"

step "/ws, through nginx, to a stand-in for the server"
# The stand-in is this image's nginx, answering every request with the WebSocket headers it was sent. It joins the
# network as $SERVER_HOST after nginx started, so reaching it also shows that nginx looks the server up per request.
cat > "$WORK/stand-in.conf" <<EOF
pid /tmp/stand-in.pid;
error_log stderr;
events {}
http {
    access_log off;
    server {
        listen $SERVER_PORT;
        location / {
            return 200 "upgrade=\$http_upgrade connection=\$http_connection\n";
        }
    }
}
EOF
podman create --name "$STAND_IN" --network "$NAME" --network-alias "$SERVER_HOST" --entrypoint nginx "$IMAGE" \
    -c /tmp/stand-in.conf -g 'daemon off;' >/dev/null || fail "couldn't create the stand-in"
podman cp "$WORK/stand-in.conf" "$STAND_IN:/tmp/stand-in.conf" || fail "couldn't copy the stand-in's configuration"
podman start "$STAND_IN" >/dev/null || fail "the stand-in didn't start"
expected="upgrade=websocket connection=upgrade"
# Twice nginx.conf's resolver valid=10s: long enough for a lookup that raced the stand-in's start to expire
deadline=$((SECONDS + 20))
while :; do
    status="$(get /ws -H 'Upgrade: websocket' -H 'Connection: Upgrade')"
    [[ "$status" == "200 "* && "$(cat "$WORK/body")" == "$expected" ]] && break
    (( SECONDS < deadline )) || break
    sleep 1
done
[[ "$status" == "200 "* ]] || fail "/ws answered '${status:-nothing}', not the stand-in's 200"
[[ "$(cat "$WORK/body")" == "$expected" ]] \
    || fail "the stand-in got '$(cat "$WORK/body")' through /ws, not '$expected'"
echo "/ws reaches $SERVER_HOST:$SERVER_PORT, which got $expected"

step "stop nginx"
podman stop -t 10 "$NAME" >/dev/null || fail "podman stop failed"
code="$(podman inspect --format '{{.State.ExitCode}}' "$NAME")"
[[ "$code" == 0 ]] || fail "nginx exited with code $code (137: still running 10s after its stop signal, so killed)"
podman logs "$NAME" > "$WORK/nginx.log" 2>&1 || fail "podman logs failed"
grep -E '\[warn\]' "$WORK/nginx.log" || true
errors="$(grep -c -E '\[(error|crit|alert|emerg)\]' "$WORK/nginx.log" || true)"
if (( errors > 0 )); then
    grep -E '\[(error|crit|alert|emerg)\]' "$WORK/nginx.log" >&2
    fail "nginx logged $errors error(s)"
fi
echo "exit code 0; nginx's log: $(wc -l < "$WORK/nginx.log") lines, no errors"

step "passed in ${SECONDS}s (the build took ${build_seconds}s)"
