#!/usr/bin/env bash
# scripts/cpu-busy.sh (#406), against a made-up topology and canned /proc/stat snapshots: which CPUs a pinned benchmark
# comparison watches, and the busiest of them.
# usage: CpuBusyTest.sh <scripts/cpu-busy.sh>
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

# Four cores of two threads, CPU n and n+4 on one core, as on the laptop that runs CI. cpu3 has only the newer name of
# the file, core_cpus_list.
sys="$work/sys"
for cpu in 0 1 2 3 4 5 6 7; do
    mkdir -p "$sys/cpu$cpu/topology"
    printf '%s,%s\n' $(( cpu % 4 )) $(( cpu % 4 + 4 )) > "$sys/cpu$cpu/topology/thread_siblings_list"
done
rm "$sys/cpu3/topology/thread_siblings_list"
echo "3,7" > "$sys/cpu3/topology/core_cpus_list"
export CPU_BUSY_SYSFS="$sys"

check "a range covers its CPUs and their siblings: 2-5 is every core" "0,1,2,3,4,5,6,7" "$(bash "$SCRIPT" watched 2-5)"
check "one CPU and its sibling" "0,4" "$(bash "$SCRIPT" watched 4)"
check "a list" "1,3,5,7" "$(bash "$SCRIPT" watched 1,3)"
check "the newer name where the older is missing" "3,7" "$(bash "$SCRIPT" watched 3)"
check "out of order, with a repeat" "0,2,4,6" "$(bash "$SCRIPT" watched 6,0,2,6)"
check "a leading zero" "0,4" "$(bash "$SCRIPT" watched 04)"
check "a CPU the topology doesn't know is watched alone" "9" "$(bash "$SCRIPT" watched 9)"
for bad in "" "x" "5-2" "1-" "1,,2" "-1" "1 2"; do
    bash "$SCRIPT" watched "$bad" > /dev/null 2>&1
    check "refuses the list '$bad'" 2 $?
done

# Each CPU's fields: user nice system idle iowait irq softirq steal guest guest_nice. Every CPU starts from the same
# counts, and half a second later (500 ticks) shows:
#   cpu0, cpu3  400 user, 100 idle: 80%
#   cpu1        50 user, 450 idle: 10%
#   cpu2        all idle: 0%
#   cpu4        250 idle, 250 iowait, which is idle too: 0%
#   cpu5        100 irq, 100 softirq, 50 steal, 250 idle: 50%
#   cpu6        100 user, of which 100 guest, 400 idle: 20%. Guest time is counted in user already; counted again, 33%.
#   cpu7        300 nice, 200 system: 100%
{
    echo "cpu  8000 0 0 72000 0 0 0 0 0 0"
    for cpu in 0 1 2 3 4 5 6 7; do
        echo "cpu$cpu 1000 0 0 9000 0 0 0 0 0 0"
    done
    echo "intr 12345 0 0"
    echo "ctxt 67890"
} > "$work/before"
cat > "$work/after" << 'EOF'
cpu  10000 300 200 74000 250 100 100 50 100 0
cpu0 1400 0 0 9100 0 0 0 0 0 0
cpu1 1050 0 0 9450 0 0 0 0 0 0
cpu2 1000 0 0 9500 0 0 0 0 0 0
cpu3 1400 0 0 9100 0 0 0 0 0 0
cpu4 1000 0 0 9250 250 0 0 0 0 0
cpu5 1000 0 0 9250 0 100 100 50 0 0
cpu6 1100 0 0 9400 0 0 0 0 100 0
cpu7 1000 300 200 9000 0 0 0 0 0 0
intr 23456 0 0
ctxt 78901
EOF
busiest() { bash "$SCRIPT" busiest "$1" "$work/before" "$work/after"; }

check "the busiest of four, a tie going to the lowest-numbered" "0 80" "$(busiest 0,1,2,3)"
check "the busiest of two, given in any order" "3 80" "$(busiest 3,1)"
check "a CPU at 10%" "1 10" "$(busiest 1,2)"
check "two idle CPUs, one of them in iowait" "2 0" "$(busiest 4,2)"
check "irq, softirq and steal are busy time" "5 50" "$(busiest 4,5)"
check "guest time is counted once" "6 20" "$(busiest 6)"
check "nice and system are busy time" "7 100" "$(busiest 6,7)"
check "the CPUs watched for a pin to 4" "0 80" "$(busiest "$(CPU_BUSY_SYSFS="$sys" bash "$SCRIPT" watched 4)")"
check "no time between the snapshots: 0%" "2 0" "$(bash "$SCRIPT" busiest 2 "$work/before" "$work/before")"
# gawk walks these eight from cpu6, so this one fails without the tie-break
check "all eight tied: the lowest-numbered" "0 0" "$(bash "$SCRIPT" busiest 0,1,2,3,4,5,6,7 "$work/before" "$work/before")"
bash "$SCRIPT" busiest 9 "$work/before" "$work/after" > /dev/null 2>&1
check "refuses a CPU the snapshots don't have" 2 $?
bash "$SCRIPT" busiest 2 "$work/before" "$work/missing" > /dev/null 2>&1
check "refuses a snapshot it can't read" 2 $?
bash "$SCRIPT" busiest 2 "$work/before" > /dev/null 2>&1
check "refuses too few arguments" 2 $?

if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "every check passed"
