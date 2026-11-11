#!/usr/bin/env bash
# ci-names.sh: the names CI's jobs give what they make in podman's store, one prefix per runner (#736).
#
# usage: scripts/ci-names.sh prefix   prints this runner's prefix, orderprocessor-ci-<runner>_
#        scripts/ci-names.sh remove   removes what an earlier job of this runner left in the store
#
# This repository's runners run on one host, as one user, so their jobs share one podman store, and run at once. Each
# name a job gives a pod, container, volume, network or image starts with its runner's prefix: the image and stack
# checks' CHECK_NAME (compose's project, and its pod_<project>), and the Postgres test databases (pg-for-tests.sh). A
# runner runs one job at a time, so whatever bears its prefix as a job starts was made by an earlier job of its own,
# which a power-off (#32) or a cancel kept from cleaning up: `remove` takes that away, with any anonymous volumes its
# containers held (#371). It never touches another runner's names, nor a name without a prefix.
#
# The runner is RUNNER_NAME, which the runner sets for every step. It may hold only [a-z0-9-], which podman's and
# compose's names allow: with no '_' in it, the '_' that ends the prefix keeps one runner's prefix from starting
# another's (orderprocessor-localhost's from orderprocessor-localhost-2's).
#
# Exit codes: 0 done; 1 a listing or a removal failed; 2 it refuses, before touching anything: a usage error, a
# RUNNER_NAME that is missing or holds anything else, or no podman.

set -uo pipefail

refuse() {
    echo "ci-names: $*" >&2
    exit 2
}

(( $# == 1 )) || refuse "usage: $0 prefix|remove"
runner=${RUNNER_NAME:-}
[[ "$runner" =~ ^[a-z0-9][a-z0-9-]*$ ]] || refuse "RUNNER_NAME '$runner' isn't a runner's name of [a-z0-9-]"
prefix="orderprocessor-ci-${runner}_"
case "$1" in
    prefix)
        echo "$prefix"
        exit 0
        ;;
    remove) ;;
    *) refuse "usage: $0 prefix|remove" ;;
esac
command -v podman > /dev/null || refuse "podman is not installed"

# This runner's names among one kind's: a pod, container, volume, network or image (its repository, under localhost/)
names_of() {
    case "$1" in
        pod) podman pod ps --format '{{.Name}}' ;;
        container) podman ps -a --format '{{.Names}}' ;;
        volume) podman volume ls --format '{{.Name}}' ;;
        network) podman network ls --format '{{.Name}}' ;;
        image) podman images --format '{{.Repository}}' | sed 's|^localhost/||' ;;
    esac | awk -v p="$prefix" 'index($0, p) == 1 || index($0, "pod_" p) == 1' | sort -u
}

failed=0
# Pods first, then their containers, and each container before its volumes and networks, and images last
for kind in pod container volume network image; do
    if ! names=$(names_of "$kind"); then
        echo "ci-names: can't list the ${kind}s" >&2
        failed=1
        continue
    fi
    [[ -n "$names" ]] || continue
    mapfile -t list <<< "$names"
    echo "ci-names: removing the ${kind}s an earlier job of $runner left: ${list[*]}"
    case "$kind" in
        pod) podman pod rm -f "${list[@]}" ;;
        container) podman rm -f -v -t 0 "${list[@]}" ;;
        volume) podman volume rm -f "${list[@]}" ;;
        network) podman network rm -f "${list[@]}" ;;
        image) podman rmi -f "${list[@]/#/localhost/}" ;;
    esac > /dev/null || {
        echo "ci-names: couldn't remove every $kind" >&2
        failed=1
    }
done
exit "$failed"
