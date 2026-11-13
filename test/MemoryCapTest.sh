#!/usr/bin/env bash
# scripts/memory-cap.sh (#607), which CI's builds (#740) and test steps and the pre-push hook's ctest run under: a
# runaway is killed at the cap and named, the rest of the run goes on, a status passes through, and with no cap to be
# had it refuses before running anything. With MEMORY_CAP_SLICE (#740) the scope goes in the slice it names, which must
# have a limit and no swap, and a kill at the slice's limit is named as the slice's. Each case makes scopes of its own,
# outside any cap this test itself runs under, and the slices it names are its own run's, stopped as it ends.
# usage: MemoryCapTest.sh <scripts/memory-cap.sh>
# Skipped (77) without python3, or on a host that can't make a systemd user scope, where the script itself refuses.
set -u
SCRIPT=$(realpath -e -- "$1") || { echo "no script $1"; exit 1; }
command -v python3 > /dev/null || { echo "skipped: no python3"; exit 77; }
if ! systemd-run --user --scope --quiet --collect true > /dev/null 2>&1; then
    echo "skipped: can't make a systemd user scope"
    exit 77
fi
# A CI runner's .env names the jobs' slice (#740), which the cases without a slice must not take
unset MEMORY_CAP_SLICE
work=$(mktemp -d)
# The slices this run names: no dash, so none is another slice's child, and the run's PID keeps them apart from those
# of another runner's run of this test
slice=memcaptest_$$.slice
bare=memcaptestbare_$$.slice
swapping=memcaptestswap_$$.slice
unknown=memcaptestunknown_$$.slice
other=memcaptestother_$$.slice
cleanup() {
    systemctl --user stop "$slice" "$bare" "$swapping" "$unknown" "$other" > /dev/null 2>&1
    rm -rf -- "${work:?}"
}
trap cleanup EXIT
failures=0

check() { # <what> <expected> <actual>
    if [[ "$2" == "$3" ]]; then
        echo "ok: $1"
    else
        echo "FAILED: $1: expected '$2', got '$3'"
        failures=$(( failures + 1 ))
    fi
}

# Has <output> a line like <pattern>? Prints yes or no
says() { # <output> <extended regex>
    if grep -q -E -- "$2" <<< "$1"; then echo yes; else echo no; fi
}

# Did the last command run? Prints yes or no, and clears the mark for the next case
ran() {
    if [[ -e "$work/ran" ]]; then echo yes; else echo no; fi
    rm -f -- "$work/ran"
}

# Makes a transient slice with the properties given as name, D-Bus type and value, as the host's unit file makes the
# jobs' slice
make_slice() { # <name> [<property> <type> <value>]...
    local name=$1
    shift
    busctl --user call org.freedesktop.systemd1 /org/freedesktop/systemd1 org.freedesktop.systemd1.Manager \
        StartTransientUnit 'ssa(sv)a(sa(sv))' "$name" fail $(( $# / 3 )) "$@" 0 > /dev/null
}

RUNAWAY='b = bytearray(256 << 20)'

# A command past the cap is killed, by SIGKILL, and the script fails with its status
out=$(bash "$SCRIPT" 64M python3 -I -c "$RUNAWAY" 2>&1)
check "a runaway is killed" 137 $?
check "and named" yes "$(says "$out" '^memory-cap: peak [0-9]+ MiB of 64M; 1 killed at the cap$')"

# OOMPolicy=continue: the kernel kills the runaway alone, and its parent goes on. The script fails all the same. The
# parent waits a second first: with stop, systemd's default, systemd would end the scope within milliseconds of the kill
out=$(bash "$SCRIPT" 64M bash -c "python3 -I -c '$RUNAWAY'; rc=\$?; sleep 1; echo \"after it: \$rc\"" 2>&1)
check "a parent whose child ran away fails" 1 $?
check "its child was killed" yes "$(says "$out" '^after it: 137$')"
check "and it went on" yes "$(says "$out" '1 killed at the cap$')"

# Under the cap the command's status passes through, and the peak is reported
out=$(bash "$SCRIPT" 64M bash -c 'exit 3' 2>&1)
check "a status passes through" 3 $?
out=$(bash "$SCRIPT" 64M true 2>&1)
check "a command that passes passes" 0 $?
check "with its peak and its scope, in its slice" yes \
    "$(says "$out" '^memory-cap: 64M, no swap, in [^/]+\.slice/run-[^/]+\.scope$')"
check "and nothing killed" yes "$(says "$out" '^memory-cap: peak [0-9]+ MiB of 64M; 0 killed at the cap$')"
MEMORY_CAP_SLICE='' bash "$SCRIPT" 64M true > /dev/null 2>&1
check "an empty MEMORY_CAP_SLICE names no slice" 0 $?

# Without the user bus there is no scope: refused, and the command never ran
env -u DBUS_SESSION_BUS_ADDRESS -u XDG_RUNTIME_DIR bash "$SCRIPT" 64M touch "$work/ran" > /dev/null 2>&1
check "no user bus: refused" 2 $?
check "and the command never ran" no "$(ran)"

# In a scope with no memory limit, the inner run refuses rather than run uncapped
systemd-run --user --scope --quiet --collect bash "$SCRIPT" --in-scope 64M touch "$work/ran" > /dev/null 2>&1
check "a scope with no limit: refused" 2 $?
check "and the command never ran" no "$(ran)"

for bad in 64 0G 4g "" -1G; do
    bash "$SCRIPT" "$bad" true > /dev/null 2>&1
    check "refuses the size '$bad'" 2 $?
done
bash "$SCRIPT" 64M > /dev/null 2>&1
check "refuses a size with no command" 2 $?

# MEMORY_CAP_SLICE (#740): the scope goes in the slice named, here one with a limit and no swap, as the jobs' slice has
make_slice "$slice" MemoryMax t $(( 128 << 20 )) MemorySwapMax t 0 || { echo "FAILED: can't make $slice"; exit 1; }
slice_re=${slice//./\\.}
out=$(MEMORY_CAP_SLICE=$slice bash "$SCRIPT" 64M cat /proc/self/cgroup 2>&1)
check "a command in the named slice passes" 0 $?
check "in a scope of that slice" yes "$(says "$out" "^0::/.+/$slice_re/run-[^/]+\.scope$")"
check "which it names" yes "$(says "$out" "^memory-cap: 64M, no swap, in $slice_re/run-[^/]+\.scope$")"

# In the slice, a runaway past its own cap is named as the cap's, and one past the slice's limit, under its own cap, as
# the slice's: there the commands in the slice together reached the limit, not this one alone
out=$(MEMORY_CAP_SLICE=$slice bash "$SCRIPT" 64M python3 -I -c "$RUNAWAY" 2>&1)
check "in the slice, a runaway past its cap is killed" 137 $?
check "and named as the cap's" yes "$(says "$out" '^memory-cap: peak [0-9]+ MiB of 64M; 1 killed at the cap$')"
out=$(MEMORY_CAP_SLICE=$slice bash "$SCRIPT" 1G python3 -I -c "$RUNAWAY" 2>&1)
check "a runaway past the slice's limit is killed" 137 $?
check "and named as the slice's" yes \
    "$(says "$out" "^memory-cap: peak [0-9]+ MiB of 1G; 1 killed at a limit above the cap: $slice_re's, ")"

# A slice with no limit, or with swap, bounds nothing, and nor does one the host lacks, which systemd makes as it's
# named, with no limits: each refused, and the command never ran
make_slice "$bare" || { echo "FAILED: can't make $bare"; exit 1; }
make_slice "$swapping" MemoryMax t $(( 128 << 20 )) || { echo "FAILED: can't make $swapping"; exit 1; }
for what in "no limit:$bare" "swap:$swapping" "a slice the host lacks:$unknown"; do
    MEMORY_CAP_SLICE=${what#*:} bash "$SCRIPT" 64M touch "$work/ran" > /dev/null 2>&1
    check "${what%%:*}: refused" 2 $?
    check "and the command never ran" no "$(ran)"
done

# A scope that landed outside the slice named, in another with a limit and no swap: refused, and the command never ran
make_slice "$other" MemoryMax t $(( 128 << 20 )) MemorySwapMax t 0 || { echo "FAILED: can't make $other"; exit 1; }
MEMORY_CAP_SLICE=$slice systemd-run --user --scope --quiet --collect --slice="$other" -p MemoryMax=64M \
    -p MemorySwapMax=0 bash "$SCRIPT" --in-scope 64M touch "$work/ran" > /dev/null 2>&1
check "a scope outside the named slice: refused" 2 $?
check "and the command never ran" no "$(ran)"

# A name that isn't a slice's is refused for its name, before anything runs. Given most of these, systemd would make a
# slice, its name escaped, and the script would refuse only once a scope was in it
# shellcheck disable=SC2016 # '$(true).slice' is a name with a command substitution in it, kept as text
for bad in jobs jobs.service jobs.slice.d a/jobs.slice ../jobs.slice -.slice -jobs.slice jobs-.slice jobs--a.slice \
    'jobs .slice' 'jobs@1.slice' '$(true).slice'; do
    out=$(MEMORY_CAP_SLICE=$bad bash "$SCRIPT" 64M touch "$work/ran" 2>&1)
    check "refuses the slice name '$bad'" 2 $?
    check "for its name" yes "$(says "$out" "^memory-cap: MEMORY_CAP_SLICE isn't a slice's name: ")"
    check "and the command never ran" no "$(ran)"
done

if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "every check passed"
