#!/usr/bin/env bash
# memory-cap.sh: runs a command under a memory cap (#607). CI's builds (#740) and test steps and the pre-push hook's
# ctest run through it, so that a build or a test that runs away is killed and named before it can take down the laptop
# the runners share.
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
# MEMORY_CAP_SLICE names the slice the scope goes in (#740): each CI runner's .env gives orderprocessor-jobs.slice,
# whose MemoryMax bounds the jobs together while each scope's cap bounds one command, outside the runners' own slice.
# The slice must have a MemoryMax and no swap, or the script refuses: systemd makes a slice it doesn't know, with no
# limits, so a slice the host lacks would bound nothing. Unset or empty, the scope goes where systemd puts it:
# app.slice.
#
# As the command ends, it prints the scope's peak memory, page cache included, and how many processes were killed: at
# the cap, or at a limit above it, the slice's (the commands in it together reached it) or the host's. A child that
# itself calls systemd-run --scope leaves the cap, for a scope of its own.
#
# Exit codes: the command's, or 1 when the command passed but a process was killed; 2 when it refuses, before running
# anything: a usage error, a slice that isn't one or has no limit, or no scope with the cap (no systemd-run, no user
# manager, no memory controller). It never runs the command uncapped.

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
    slice_cg=${cg%/*}
    slice=${slice_cg##*/}
    if [[ -n "${MEMORY_CAP_SLICE:-}" ]]; then
        [[ "$slice" == "$MEMORY_CAP_SLICE" ]] ||
            refuse "the scope is in $slice, not in MEMORY_CAP_SLICE's $MEMORY_CAP_SLICE"
        slice_max=$(cat "$slice_cg/memory.max" 2> /dev/null) || slice_max=unknown
        slice_swap=$(cat "$slice_cg/memory.swap.max" 2> /dev/null) || slice_swap=unknown
        [[ "$slice_max" =~ ^[0-9]+$ && "$slice_swap" == 0 ]] ||
            refuse "$slice has memory.max $slice_max and memory.swap.max $slice_swap: what runs in it is unbounded"
    fi
    echo "memory-cap: $size, no swap, in $slice/${cg##*/}"
    "$@"
    rc=$?
    killed=$(awk '$1 == "oom_kill" { print $2 }' "$cg/memory.events")
    # A kill counts in the victim's scope whichever limit was reached, but only one at the scope's own cap counts an oom
    ooms=$(awk '$1 == "oom" { print $2 }' "$cg/memory.events")
    peak=$(cat "$cg/memory.peak" 2> /dev/null) || peak=0
    where="at the cap"
    (( killed > 0 && ooms == 0 )) &&
        where="at a limit above the cap: $slice's, which the commands in it share, or the host's"
    echo "memory-cap: peak $(( peak / 1048576 )) MiB of $size; $killed killed $where"
    if (( killed > 0 )); then
        [[ "${GITHUB_ACTIONS:-}" == true ]] && echo "::error::memory-cap: $killed process(es) killed $where ($size)"
        (( rc == 0 )) && rc=1
    fi
    exit "$rc"
fi

(( $# >= 2 )) || refuse "usage: $0 SIZE COMMAND [ARG]..."
size=$1
shift
[[ "$size" =~ ^[1-9][0-9]*[KMGT]$ ]] || refuse "SIZE is a number and K, M, G or T: '$size'"
slice_arg=()
if [[ -n "${MEMORY_CAP_SLICE:-}" ]]; then
    # A slice's own name: no path, no template, no escape, and each of the levels its dashes give named
    [[ "$MEMORY_CAP_SLICE" =~ ^[A-Za-z0-9_.:]+(-[A-Za-z0-9_.:]+)*\.slice$ ]] ||
        refuse "MEMORY_CAP_SLICE isn't a slice's name: '$MEMORY_CAP_SLICE'"
    slice_arg=(--slice="$MEMORY_CAP_SLICE")
fi
command -v systemd-run > /dev/null || refuse "no systemd-run, so no cap: the command would run uncapped"
# A scope first, alone, so that a failure to make one is never taken for the command's
why=$(systemd-run --user --scope --quiet --collect "${slice_arg[@]}" true 2>&1) ||
    refuse "can't make a systemd user scope: $why"
exec systemd-run --user --scope --quiet --collect "${slice_arg[@]}" -p MemoryMax="$size" -p MemorySwapMax=0 \
    -p OOMPolicy=continue -- "$BASH" "$(realpath -e -- "$0")" --in-scope "$size" "$@"
