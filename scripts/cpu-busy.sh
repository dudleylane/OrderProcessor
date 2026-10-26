#!/usr/bin/env bash
# The CPUs a pinned benchmark run shares with whatever else runs, and how busy they are (#406). The one-minute load
# average can't say which CPUs its load is on: a job pinned to CPUs 1-3 held it near 2, under benchmark-regression.sh's
# settle, while it saturated the CPUs a comparison pinned to 2-5 ran on. So the comparison also watches the pinned CPUs
# themselves and their hyperthread siblings, which share a core with them.
#
# usage: cpu-busy.sh watched <cpu list>
#            The CPUs of a taskset list (2-5, 0,2, 1-3,6) and the hyperthread siblings of each, in order and
#            comma-separated
#        cpu-busy.sh busiest <cpus> <stat before> <stat after>
#            Of the comma-separated CPUs, the busiest between two snapshots of /proc/stat, and the share of that time it
#            was busy: "<cpu> <percent>", the percent rounded down
# The siblings are read from /sys/devices/system/cpu, or from CPU_BUSY_SYSFS where that is set (the test sets it).
set -euo pipefail

sysfs=${CPU_BUSY_SYSFS:-/sys/devices/system/cpu}

usage() {
    echo "usage: $0 watched <cpu list> | busiest <cpus> <stat before> <stat after>" >&2
    exit 2
}

# The CPUs of a taskset-style list, one a line
expand() {
    local part lo hi
    local -a parts
    [[ -n "$1" ]] || { echo "not a CPU list: '$1'" >&2; exit 2; }
    IFS=, read -r -a parts <<< "$1"
    for part in "${parts[@]}"; do
        if [[ "$part" =~ ^([0-9]+)-([0-9]+)$ ]]; then
            lo=$(( 10#${BASH_REMATCH[1]} ))
            hi=$(( 10#${BASH_REMATCH[2]} ))
            (( lo <= hi )) || { echo "not a CPU list: '$1'" >&2; exit 2; }
            seq "$lo" "$hi"
        elif [[ "$part" =~ ^[0-9]+$ ]]; then
            echo $(( 10#$part ))
        else
            echo "not a CPU list: '$1'" >&2
            exit 2
        fi
    done
}

watched() {
    local cpu file cpus
    cpus=$(expand "$1")
    for cpu in $cpus; do
        echo "$cpu"
        # thread_siblings_list is the older name of core_cpus_list; a kernel has one or both
        file="$sysfs/cpu$cpu/topology/thread_siblings_list"
        [[ -r "$file" ]] || file="$sysfs/cpu$cpu/topology/core_cpus_list"
        if [[ -r "$file" ]]; then
            expand "$(tr -d ' \n' < "$file")"
        fi
    done | sort -n -u | paste -s -d, -
}

busiest() {
    [[ -r "$2" && -r "$3" ]] || { echo "can't read the snapshots $2 and $3" >&2; exit 2; }
    awk -v cpus="$1" '
        BEGIN { n = split(cpus, list, ","); for (i = 1; i <= n; i++) { want["cpu" list[i]] = 1 } }
        FNR == 1 { file++ }
        # cpuN user nice system idle iowait irq softirq steal guest guest_nice; guest time is in user and nice already
        ($1 in want) {
            total = $2 + $3 + $4 + $5 + $6 + $7 + $8 + $9
            idle = $5 + $6
            if (1 == file) { t0[$1] = total; i0[$1] = idle } else { t1[$1] = total; i1[$1] = idle }
        }
        END {
            best = ""; bestPct = -1
            for (c in want) {
                if (!(c in t0) || !(c in t1)) { print "no " c " in the snapshots" > "/dev/stderr"; exit 2 }
                dt = t1[c] - t0[c]; di = i1[c] - i0[c]
                pct = (dt > 0) ? int(100 * (dt - di) / dt) : 0
                # the lowest-numbered of the busiest, so the answer does not depend on the order awk walks the CPUs
                if (pct > bestPct || (pct == bestPct && substr(c, 4) + 0 < substr(best, 4) + 0)) { best = c; bestPct = pct }
            }
            print substr(best, 4), bestPct
        }
    ' "$2" "$3"
}

case "${1:-}" in
    watched)
        [[ $# -eq 2 ]] || usage
        watched "$2" ;;
    busiest)
        [[ $# -eq 4 ]] || usage
        busiest "$2" "$3" "$4" ;;
    *)
        usage ;;
esac
