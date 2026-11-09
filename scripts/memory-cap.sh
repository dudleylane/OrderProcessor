#!/usr/bin/env bash
# memory-cap.sh: runs a command under a memory cap (#607). CI's test steps and the pre-push hook run ctest through it,
# so that a test that runs away is killed and named before it can take down the laptop the runners share.
#
# usage: scripts/memory-cap.sh SIZE COMMAND [ARG]...
#
# The command runs in a transient systemd user scope with MemoryMax=SIZE (systemd's syntax: 4G, 512M) and:
#   MemorySwapMax=0     with swap, the kernel would push a runaway into the host's swap, slowing every runner, rather
#                       than stop it
#   OOMPolicy=continue  the kernel kills the largest process in the scope at the cap, the runaway test, and the rest go
#                       on. With systemd's default, stop, the first kill ends the whole scope, ctest with it, and the
#                       log never names the test
# A cgroup counts what the processes use, together. An address-space limit (ulimit -v) would count what each one
# reserves, which LMDB's maps and the threads' arenas and stacks make 8 to 19 times what the tests use here, and
# ASan's and TSan's shadow memory makes terabytes.
#
# As the command ends, it prints the scope's peak memory, page cache included, and how many processes the cap killed.
# A child that itself calls systemd-run --scope leaves the cap, for a scope of its own.
#
# Exit codes: the command's, or 1 when the command passed but a process was killed at the cap; 2 when it refuses,
# before running anything: a usage error, or no scope with the cap (no systemd-run, no user manager, no memory
# controller). It never runs the command uncapped.

set -uo pipefail

refuse() {
    echo "memory-cap: $*" >&2
    exit 2
}

# Inside the scope, which the call below makes: check the cap holds, run the command, and report
if [[ "${1:-}" == --in-scope ]]; then
    size=${2:?}
    shift 2
    cg=/sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup)
    max=$(cat "$cg/memory.max" 2> /dev/null) || refuse "$cg has no memory controller: the command would run uncapped"
    swap=$(cat "$cg/memory.swap.max" 2> /dev/null) || swap=unknown
    [[ "$max" != max && "$swap" == 0 ]] ||
        refuse "$cg has memory.max $max and memory.swap.max $swap: the command would run uncapped"
    echo "memory-cap: $size, no swap, in ${cg##*/}"
    "$@"
    rc=$?
    killed=$(awk '$1 == "oom_kill" { print $2 }' "$cg/memory.events")
    peak=$(cat "$cg/memory.peak" 2> /dev/null) || peak=0
    echo "memory-cap: peak $(( peak / 1048576 )) MiB of $size; $killed killed at the cap"
    if (( killed > 0 )); then
        [[ "${GITHUB_ACTIONS:-}" == true ]] && echo "::error::memory-cap: $killed process(es) killed at the $size cap"
        (( rc == 0 )) && rc=1
    fi
    exit "$rc"
fi

(( $# >= 2 )) || refuse "usage: $0 SIZE COMMAND [ARG]..."
size=$1
shift
[[ "$size" =~ ^[1-9][0-9]*[KMGT]$ ]] || refuse "SIZE is a number and K, M, G or T: '$size'"
command -v systemd-run > /dev/null || refuse "no systemd-run, so no cap: the command would run uncapped"
# A scope first, alone, so that a failure to make one is never taken for the command's
why=$(systemd-run --user --scope --quiet --collect true 2>&1) || refuse "can't make a systemd user scope: $why"
exec systemd-run --user --scope --quiet --collect -p MemoryMax="$size" -p MemorySwapMax=0 -p OOMPolicy=continue \
    -- "$BASH" "$(realpath -e -- "$0")" --in-scope "$size" "$@"
