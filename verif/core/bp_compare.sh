#!/bin/sh
# Runs programs on two simulator builds and requires identical results.
#
#   bp_compare.sh SIM_A SIM_B LOGDIR "TIMED" ELF...
#
# Branch prediction must never change architectural behavior, so the two
# predictor builds (soc_sim and soc_sim_bp0) must give the same commit log,
# UART output and exit code for every program. The programs named in TIMED
# (space separated) read something that depends on time, the cycle counter
# or the NPU's BUSY flag while polling, so their commit logs may differ and
# only their output and exit code are compared. Matching logs are deleted
# as soon as they are compared, to keep the disk footprint small. Exits
# nonzero if any program differs, or if an ELF is missing (so an empty or
# stale test list cannot pass by running nothing).
set -u

MAX_CYCLES=50000000   # the longest program here needs about 13 M

sim_a=$1
sim_b=$2
dir=$3
timed=" $4 "
shift 4
mkdir -p "$dir"

same=0
differ=0
for elf in "$@"; do
    if [ ! -f "$elf" ]; then
        echo "FAIL $elf: no such program"
        differ=$((differ + 1))
        continue
    fi
    test=$(basename "$elf" .elf)
    for sim in "$sim_a" "$sim_b"; do
        name=$(basename "$sim")
        "$sim" --quiet --max-cycles $MAX_CYCLES --commit-log "$dir/$test.$name.commit" "$elf" \
            > "$dir/$test.$name.out" 2> /dev/null
        echo $? > "$dir/$test.$name.exit"
    done
    a=$dir/$test.$(basename "$sim_a")
    b=$dir/$test.$(basename "$sim_b")
    logs_match=yes
    case $timed in
        *" $test "*) ;;
        *) cmp -s "$a.commit" "$b.commit" || logs_match=no ;;
    esac
    if [ $logs_match = yes ] && cmp -s "$a.out" "$b.out" && cmp -s "$a.exit" "$b.exit"; then
        same=$((same + 1))
        rm -f -- "${a:?}.commit" "${b:?}.commit"
    else
        echo "FAIL $test: $(basename "$sim_a") and $(basename "$sim_b") differ (logs: $a.*, $b.*)"
        differ=$((differ + 1))
    fi
done
echo "predictor builds compared: $same programs identical, $differ differ"
[ "$differ" -eq 0 ] && [ "$same" -gt 0 ]
