#!/usr/bin/env bash
# check-server-image.sh: build the server's Docker image and check that it works (#135).
#
# Nothing else builds the image. CI and the pre-push hook build on the host's own
# toolchain, so the image couldn't build on either branch for months while every
# check stayed green (#132). This builds it the way `docker compose up --build` does,
# from docker/Dockerfile.oms-server with the repository as context, and then:
#   1. seedData seeds a new volume.
#   2. The server starts on it with the image's own CMD.
#   3. Every port the image EXPOSEs is listened on: 8080, the WebSocket port, and on
#      feature/fx-swap-support also 5001, the FIX acceptor's. This is checked inside the
#      container, because rootless port forwarding accepts a connection on the host
#      whether or not anything listens behind it.
#   4. /ws, through the published port, answers a WebSocket upgrade with 101.
#   5. SIGTERM stops the server with exit code 0, the clean stop that saves the next
#      id (#81), and its exchange.log has no errors.
# The image, container and volume it creates are removed when it exits, pass or fail.
# The base image stays, so the next build only checks it for an update.
#
# CI runs this on every push (.github/workflows/docker.yml). Run it the same way
# before changing the Dockerfile, the build or a dependency:
#   scripts/check-server-image.sh
# It needs podman and network access (dnf mirrors, quay.io, github.com), and takes a
# few minutes, because every run builds from scratch.
#
# Environment:
#   CHECK_NAME     names the image (localhost/$CHECK_NAME), the container and the
#                  volume (default: orderprocessor-check-<pid>)
#   START_TIMEOUT  seconds the server may take to listen on every port (default: 60)
#
# Exit codes: 0 = the image built and passed every check, 1 = it didn't,
# 2 = usage or setup error

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${CHECK_NAME:-orderprocessor-check-$$}"
IMAGE="localhost/$NAME"
START_TIMEOUT="${START_TIMEOUT:-60}"
WS_PORT=8080 # the port the image's CMD gives the WebSocket server

usage_error() {
    echo "check-server-image: $1" >&2
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
WORK="$(mktemp -d)"

step() {
    echo
    echo "=== check-server-image: $1 ==="
}

fail() {
    echo >&2
    echo "check-server-image: FAILED: $1" >&2
    exit 1
}

# On any exit: show what the server said if a check failed, then remove what this run made.
cleanup() {
    local status=$?
    if (( status != 0 )) && podman container exists "$NAME" 2>/dev/null; then
        step "the server's output"
        podman logs "$NAME" 2>&1 | tail -n 50 || true
        if [[ -f "$WORK/exchange.log" ]] || podman cp "$NAME:/exchange.log" "$WORK/" >/dev/null 2>&1; then
            step "the end of its exchange.log"
            tail -n 50 "$WORK/exchange.log"
        fi
    fi
    podman rm -f -t 0 "$NAME" >/dev/null 2>&1 || true
    podman volume rm -f "$NAME" >/dev/null 2>&1 || true
    podman rmi -f "$IMAGE" >/dev/null 2>&1 || true
    rm -rf "${WORK:?}"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Prints the status line the server answers a WebSocket upgrade request to /ws with.
ws_upgrade() {
    local line=""
    exec 3<>"/dev/tcp/$1/$2" || return 1
    printf 'GET /ws HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n' "$1" "$2" >&3
    IFS= read -r -t 5 line <&3 || true
    exec 3<&-
    printf '%s\n' "${line%$'\r'}"
}

cd "$ROOT"

step "build $IMAGE from docker/Dockerfile.oms-server ($(podman --version))"
# --layers=false keeps no intermediate images, so a run leaves nothing behind in the
# store it shares with everything else this user runs, and every run installs current
# packages. --pull=newer refreshes the base image, and uses the local copy when the
# registry can't be reached.
podman build --layers=false --pull=newer -f docker/Dockerfile.oms-server -t "$IMAGE" . \
    || fail "the image didn't build"
build_seconds=$SECONDS

step "seed a volume with seedData"
podman run --rm -v "$NAME:/data" --entrypoint seedData "$IMAGE" --data-dir /data || fail "seedData failed"

mapfile -t ports < <(podman image inspect --format '{{range $p, $_ := .Config.ExposedPorts}}{{println $p}}{{end}}' \
    "$IMAGE" | sed '/^$/d')
for port in "${ports[@]}"; do
    [[ "$port" == */tcp ]] || fail "the image exposes $port, and this script checks TCP ports only"
done
[[ " ${ports[*]} " == *" $WS_PORT/tcp "* ]] \
    || fail "the image doesn't expose $WS_PORT/tcp, the WebSocket port its CMD gives the server"

step "start the server and wait for it to listen on ${ports[*]}"
podman run -d --name "$NAME" -v "$NAME:/data" -p "127.0.0.1::$WS_PORT" "$IMAGE" >/dev/null \
    || fail "the container didn't start"
deadline=$((SECONDS + START_TIMEOUT))
while :; do
    [[ "$(podman inspect --format '{{.State.Running}}' "$NAME")" == true ]] \
        || fail "the server exited at start, with code $(podman inspect --format '{{.State.ExitCode}}' "$NAME")"
    waiting=()
    for port in "${ports[@]}"; do
        podman exec "$NAME" timeout 2 bash -c "exec 3<>/dev/tcp/127.0.0.1/${port%/tcp}" 2>/dev/null \
            || waiting+=("$port")
    done
    (( ${#waiting[@]} == 0 )) && break
    (( SECONDS < deadline )) || fail "after ${START_TIMEOUT}s nothing listens on ${waiting[*]}"
    sleep 1
done
echo "listening on ${ports[*]}"

step "WebSocket upgrade on /ws"
published="$(podman port "$NAME" "$WS_PORT/tcp" | head -n 1)"
status=""
for _ in 1 2 3 4 5; do
    status="$(ws_upgrade "${published%:*}" "${published##*:}")" || true
    [[ "$status" == "HTTP/1.1 101"* ]] && break
    sleep 1
done
[[ "$status" == "HTTP/1.1 101"* ]] || fail "/ws on $published answered '${status:-nothing}', not 101"
echo "$published/ws: $status"

step "stop the server with SIGTERM"
podman stop -t 30 "$NAME" >/dev/null || fail "podman stop failed"
code="$(podman inspect --format '{{.State.ExitCode}}' "$NAME")"
[[ "$code" == 0 ]] || fail "the server exited with code $code (137: still running 30s after SIGTERM, so killed)"
podman cp "$NAME:/exchange.log" "$WORK/" >/dev/null || fail "the container has no /exchange.log"
errors="$(grep -c -E ': \[(Error|Fatal)\]\[' "$WORK/exchange.log" || true)"
# The logger reports its own settings at Warn level ("Debug logging turned off."); leave those out.
grep -E ': \[Warn\]\[' "$WORK/exchange.log" | grep -v -E '\] [A-Za-z]+ logging turned (on|off)\.$' \
    > "$WORK/warnings" || true
warnings="$(wc -l < "$WORK/warnings")"
cat "$WORK/warnings"
if (( errors > 0 )); then
    grep -E ': \[(Error|Fatal)\]\[' "$WORK/exchange.log" >&2
    fail "the server logged $errors error(s)"
fi
echo "exit code 0; exchange.log: $(wc -l < "$WORK/exchange.log") lines, $warnings warning(s), no errors"

step "passed in ${SECONDS}s (the build took ${build_seconds}s)"
