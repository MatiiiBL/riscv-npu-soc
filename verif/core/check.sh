#!/bin/sh
# Runs one directed test on one simulator and checks the outcome.
#
#   check.sh SIM ELF EXPECTED_EXIT EXPECTED_STDOUT LOGDIR ISS NONZERO
#
# The run passes when the simulator exits with EXPECTED_EXIT, its stdout (the
# UART bytes) equals the file EXPECTED_STDOUT, or is empty when that is "-",
# the commit log has exactly as many lines as the instret value in the
# summary line, and the --stats counters are all present and consistent
# (cycles >= instret, mispredicts <= branches+jumps). NONZERO lists, comma
# separated, the counters this test must drive above zero (load-use,
# mispredicts, divider), or is "-"; only "above zero" is checked, so the
# tests do not depend on exact hazard costs. Unless ISS is "-", the program
# also runs through tools/difftest.py, which requires the commit log, UART
# output and exit code to match the ISS exactly. Logs are kept in LOGDIR as
# <test>.<sim>.{out,err,commit} and <test>.<sim>.difftest/.
set -u

sim=$1
elf=$2
want=$3
want_out=$4
dir=$5
iss=$6
nonzero=$7

repo=$(cd "$(dirname "$0")/../.." && pwd)
test=$(basename "$elf" .elf)
name=$(basename "$sim")
base=$dir/$test.$name

# Value of one --stats counter, e.g. stat "load-use stalls".
stat() {
    sed -n "s/^\[rtl\] stats: $1  *\([0-9][0-9]*\).*/\1/p" "$base.err"
}

check_stats() {
    cycles=$(stat cycles)
    instret=$(stat instret)
    load_use=$(stat "load-use stalls")
    ctrl=$(stat "branches+jumps")
    mispredicts=$(stat mispredicts)
    divider=$(stat "divider stalls")
    for v in "$cycles" "$instret" "$load_use" "$ctrl" "$mispredicts" "$divider"; do
        [ -n "$v" ] || { echo "a --stats counter is missing"; return; }
    done
    [ "$cycles" -ge "$instret" ] || { echo "stats: cycles $cycles < instret $instret"; return; }
    [ "$mispredicts" -le "$ctrl" ] || { echo "stats: $mispredicts mispredicts > $ctrl branches"; return; }
    for counter in $(echo "$nonzero" | tr ',' ' '); do
        case $counter in
            -) ;;
            load-use) [ "$load_use" -gt 0 ] || echo "stats: no load-use stalls" ;;
            mispredicts) [ "$mispredicts" -gt 0 ] || echo "stats: no mispredicts" ;;
            divider) [ "$divider" -gt 0 ] || echo "stats: no divider stall cycles" ;;
            *) echo "unknown counter '$counter' in the test list" ;;
        esac
    done
}

"$sim" --stats --commit-log "$base.commit" "$elf" > "$base.out" 2> "$base.err"
got=$?

reason=""
if [ "$got" -ne "$want" ]; then
    reason="exit code $got, expected $want"
elif [ "$want_out" = "-" ] && [ -s "$base.out" ]; then
    reason="unexpected UART output"
elif [ "$want_out" != "-" ] && ! cmp -s "$want_out" "$base.out"; then
    reason="UART output differs from $want_out"
else
    instret=$(sed -n 's/.*instret=\([0-9]*\).*/\1/p' "$base.err")
    lines=$(wc -l < "$base.commit" | tr -d ' ')
    if [ "$instret" != "$lines" ]; then
        reason="commit log has $lines lines but instret=$instret"
    else
        reason=$(check_stats | head -1)
    fi
fi
if [ -z "$reason" ] && [ "$iss" != "-" ]; then
    if ! "${PYTHON:-python3}" "$repo/tools/difftest.py" -q --rtl "$sim" --iss "$iss" \
            --expect-exit "$want" --work-dir "$base.difftest" "$elf" > "$base.difftest.txt" 2>&1; then
        reason="differs from the ISS (see $base.difftest.txt)"
    fi
fi

if [ -n "$reason" ]; then
    echo "FAIL $test on $name: $reason (logs: $base.*)"
    exit 1
fi
echo "PASS $test on $name"
