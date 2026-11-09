#!/usr/bin/env bash
# scripts/memory-cap.sh (#607), which CI's test steps and the pre-push hook run ctest under: a runaway is killed at the
# cap and named, the rest of the run goes on, a status passes through, and with no cap to be had it refuses before
# running anything. Each case makes scopes of its own, outside any cap this test itself runs under.
# usage: MemoryCapTest.sh <scripts/memory-cap.sh>
# Skipped (77) without python3, or on a host that can't make a systemd user scope, where the script itself refuses.
set -u
SCRIPT=$(realpath -e -- "$1") || { echo "no script $1"; exit 1; }
command -v python3 > /dev/null || { echo "skipped: no python3"; exit 77; }
if ! systemd-run --user --scope --quiet --collect true > /dev/null 2>&1; then
    echo "skipped: can't make a systemd user scope"
    exit 77
fi
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

# Has <output> a line like <pattern>? Prints yes or no
says() { # <output> <extended regex>
    if grep -q -E -- "$2" <<< "$1"; then echo yes; else echo no; fi
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
check "with its peak and its scope" yes "$(says "$out" '^memory-cap: 64M, no swap, in run-.*\.scope$')"
check "and nothing killed" yes "$(says "$out" '^memory-cap: peak [0-9]+ MiB of 64M; 0 killed at the cap$')"

# Without the user bus there is no scope: refused, and the command never ran
env -u DBUS_SESSION_BUS_ADDRESS -u XDG_RUNTIME_DIR bash "$SCRIPT" 64M touch "$work/ran" > /dev/null 2>&1
check "no user bus: refused" 2 $?
check "and the command never ran" no "$([[ -e "$work/ran" ]] && echo yes || echo no)"

# In a scope with no memory limit, the inner run refuses rather than run uncapped
systemd-run --user --scope --quiet --collect bash "$SCRIPT" --in-scope 64M touch "$work/ran" > /dev/null 2>&1
check "a scope with no limit: refused" 2 $?
check "and the command never ran" no "$([[ -e "$work/ran" ]] && echo yes || echo no)"

for bad in 64 0G 4g "" -1G; do
    bash "$SCRIPT" "$bad" true > /dev/null 2>&1
    check "refuses the size '$bad'" 2 $?
done
bash "$SCRIPT" 64M > /dev/null 2>&1
check "refuses a size with no command" 2 $?

if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "every check passed"
