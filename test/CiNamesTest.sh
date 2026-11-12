#!/usr/bin/env bash
# scripts/ci-names.sh (#736), against a podman that answers from a made-up store which two runners' jobs, older names
# and other projects share. A runner's cleanup removes its own runner's pods, containers, volumes, networks and images,
# and nothing else: not the names of a runner whose name starts with its own (orderprocessor-localhost-2 beside
# orderprocessor-localhost). A runner's name it can't vouch for is refused before podman is asked anything.
# usage: CiNamesTest.sh <scripts/ci-names.sh>
set -u
SCRIPT=$(realpath -e -- "$1") || { echo "no script $1"; exit 1; }
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

# A podman that lists $FAKE_PODMAN/store/<kind> and removes from it. It notes each call in $FAKE_PODMAN/calls and each
# name it removes in $FAKE_PODMAN/removed, as "<kind> <name>". It fails any call shaped otherwise than the script's,
# and every call of a command for which $FAKE_PODMAN/fail-<command> exists.
mkdir -p "$work/bin"
cat > "$work/bin/podman" << 'EOF'
#!/usr/bin/env bash
store=$FAKE_PODMAN/store
echo "$*" >> "$FAKE_PODMAN/calls"
[[ -e "$FAKE_PODMAN/fail-$1" ]] && exit 125
take() { # <kind> <name>...
    local kind=$1 name
    shift
    for name; do
        echo "$kind $name" >> "$FAKE_PODMAN/removed"
        grep -vxF -- "$name" "$store/$kind" > "$store/$kind.new"
        mv "$store/$kind.new" "$store/$kind"
    done
}
case "$*" in
    "pod ps --format {{.Name}}") cat "$store/pod" ;;
    "ps -a --format {{.Names}}") cat "$store/container" ;;
    "volume ls --format {{.Name}}") cat "$store/volume" ;;
    "network ls --format {{.Name}}") cat "$store/network" ;;
    "images --format {{.Repository}}") cat "$store/image" ;;
    "pod rm -f "*) shift 3; take pod "$@" ;;
    "rm -f -v -t 0 "*) shift 5; take container "$@" ;;
    "volume rm -f "*) shift 3; take volume "$@" ;;
    "network rm -f "*) shift 3; take network "$@" ;;
    "rmi -f "*) shift 2; take image "$@" ;;
    *) echo "podman: unexpected call: $*" >&2; exit 99 ;;
esac
EOF
chmod 755 "$work/bin/podman"
export FAKE_PODMAN="$work/fake"
export PATH="$work/bin:$PATH"

A=orderprocessor-ci-orderprocessor-localhost_
B=orderprocessor-ci-orderprocessor-localhost-2_
# The store: each runner's leftovers of every kind (compose's pod, network and volume carry its project's name), the
# names CI gave before #736, a local check's, other projects', and anonymous ones. An image with two tags lists twice.
fresh_store() {
    rm -rf "$FAKE_PODMAN"
    mkdir -p "$FAKE_PODMAN/store"
    printf '%s\n' "pod_${A}9-1-stack" "pod_${B}9-1-stack" pod_quantlib > "$FAKE_PODMAN/store/pod"
    printf '%s\n' "${A}pg-release" "${A}9-1-frontend-server" "${A}9-1-stack_oms-db_1" "${B}pg-tsan" "${B}9-1" \
        orderprocessor-ci-pg-release orderprocessor-ci-35000-1 orderprocessor-check-1234 quantlib-db \
        > "$FAKE_PODMAN/store/container"
    printf '%s\n' "${A}9-1" "${A}9-1-stack_oms-data" "${B}9-1" 9e2060a9317f451ffa41728d350e0e23 \
        > "$FAKE_PODMAN/store/volume"
    printf '%s\n' "${A}9-1-frontend" "${A}9-1-stack_default" "${B}9-1-frontend" podman > "$FAKE_PODMAN/store/network"
    printf '%s\n' "localhost/${A}9-1" "localhost/${A}9-1" "localhost/${A}9-1-stack_oms-server" "localhost/${B}9-1" \
        localhost/orderprocessor-ci-35000-1 docker.io/library/postgres '<none>' > "$FAKE_PODMAN/store/image"
}
calls() { cat "$FAKE_PODMAN/calls" 2> /dev/null | wc -l; }
removed() { sort "$FAKE_PODMAN/removed" 2> /dev/null | paste -s -d ' '; }
left() { cat "$FAKE_PODMAN/store/$1" | paste -s -d ' '; }

# Each runner's prefix, ended by the '_' that no runner's name holds
fresh_store
check "a runner's prefix" "$A" "$(RUNNER_NAME=orderprocessor-localhost bash "$SCRIPT" prefix)"
check "the second runner's" "$B" "$(RUNNER_NAME=orderprocessor-localhost-2 bash "$SCRIPT" prefix)"
check "the prefix asks podman nothing" 0 "$(calls)"

# The first runner's cleanup takes its own leftovers of every kind, each image once, and nothing of the second's,
# whose names start with its runner's name
out=$(RUNNER_NAME=orderprocessor-localhost bash "$SCRIPT" remove 2>&1)
check "the first runner's cleanup passes" 0 $?
check "and takes its own leftovers alone" \
    "$(printf '%s\n' "container ${A}9-1-frontend-server" "container ${A}9-1-stack_oms-db_1" "container ${A}pg-release" \
        "image localhost/${A}9-1" "image localhost/${A}9-1-stack_oms-server" "network ${A}9-1-frontend" \
        "network ${A}9-1-stack_default" "pod pod_${A}9-1-stack" "volume ${A}9-1" "volume ${A}9-1-stack_oms-data" |
        sort | paste -s -d ' ')" \
    "$(removed)"
check "it leaves the other pods" "pod_${B}9-1-stack pod_quantlib" "$(left pod)"
others="${B}pg-tsan ${B}9-1 orderprocessor-ci-pg-release orderprocessor-ci-35000-1"
others+=" orderprocessor-check-1234 quantlib-db"
check "the other containers" "$others" "$(left container)"
check "the other volumes" "${B}9-1 9e2060a9317f451ffa41728d350e0e23" "$(left volume)"
check "the other networks" "${B}9-1-frontend podman" "$(left network)"
check "the other images" "localhost/${B}9-1 localhost/orderprocessor-ci-35000-1 docker.io/library/postgres <none>" \
    "$(left image)"
said='^ci-names: removing the [a-z]*s an earlier job of orderprocessor-localhost left: '
check "and names what it removed" 5 "$(grep -c "$said" <<< "$out")"

# The second runner's takes its own, and nothing of the first's
fresh_store
RUNNER_NAME=orderprocessor-localhost-2 bash "$SCRIPT" remove > /dev/null 2>&1
check "the second runner's cleanup passes" 0 $?
check "and takes its own leftovers alone" \
    "$(printf '%s\n' "container ${B}9-1" "container ${B}pg-tsan" "image localhost/${B}9-1" "network ${B}9-1-frontend" \
        "pod pod_${B}9-1-stack" "volume ${B}9-1" | paste -s -d ' ')" \
    "$(removed)"

# With nothing of its own in the store, it removes nothing
rm -f "$FAKE_PODMAN/removed" "$FAKE_PODMAN/calls"
RUNNER_NAME=orderprocessor-localhost-3 bash "$SCRIPT" remove > /dev/null 2>&1
check "a runner with nothing left passes" 0 $?
check "and removes nothing" "" "$(removed)"
check "having only listed the five kinds" 5 "$(calls)"

# A listing that fails fails the cleanup, which still takes the other kinds' leftovers
fresh_store
touch "$FAKE_PODMAN/fail-volume"
RUNNER_NAME=orderprocessor-localhost bash "$SCRIPT" remove > /dev/null 2>&1
check "a failed listing fails it" 1 $?
check "after it took the containers" "$others" "$(left container)"
rm -f "$FAKE_PODMAN/fail-volume"

# A removal that fails fails it too
fresh_store
touch "$FAKE_PODMAN/fail-rmi"
RUNNER_NAME=orderprocessor-localhost bash "$SCRIPT" remove > /dev/null 2>&1
check "a failed removal fails it" 1 $?
rm -f "$FAKE_PODMAN/fail-rmi"

# A runner's name that podman's names can't carry, or that holds the '_' that ends a prefix, is refused before podman
# is asked anything; so is a missing one, and a usage error
for name in '' Orderprocessor-localhost orderprocessor_localhost -localhost 'orderprocessor localhost' 'a/b'; do
    fresh_store
    RUNNER_NAME="$name" bash "$SCRIPT" remove > /dev/null 2>&1
    check "RUNNER_NAME '$name' is refused" 2 $?
    check "before podman is asked" 0 "$(calls)"
done
fresh_store
env -u RUNNER_NAME bash "$SCRIPT" prefix > /dev/null 2>&1
check "a missing RUNNER_NAME is refused" 2 $?
for args in '' 'remove now' 'list'; do
    # shellcheck disable=SC2086 # each word an argument
    RUNNER_NAME=orderprocessor-localhost bash "$SCRIPT" $args > /dev/null 2>&1
    check "arguments '$args' are refused" 2 $?
done
check "before podman is asked" 0 "$(calls)"

(( failures == 0 )) || { echo "$failures failed"; exit 1; }
echo "all passed"
