#!/bin/sh
# Runs one directed test on one simulator and checks the outcome.
#
#   check.sh SIM ELF EXPECTED_EXIT EXPECTED_STDOUT LOGDIR
#
# The run passes when the simulator exits with EXPECTED_EXIT, its stdout (the
# UART bytes) equals the file EXPECTED_STDOUT, or is empty when that is "-",
# and the commit log has exactly as many lines as the instret value in the
# summary line. Logs are kept in LOGDIR as <test>.<sim>.{out,err,commit}.
set -u

sim=$1
elf=$2
want=$3
want_out=$4
dir=$5

test=$(basename "$elf" .elf)
name=$(basename "$sim")
base=$dir/$test.$name

"$sim" --commit-log "$base.commit" "$elf" > "$base.out" 2> "$base.err"
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
    fi
fi

if [ -n "$reason" ]; then
    echo "FAIL $test on $name: $reason (logs: $base.*)"
    exit 1
fi
echo "PASS $test on $name"
