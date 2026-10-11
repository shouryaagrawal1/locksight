#!/usr/bin/env bash
# overhead.sh - how much slower is a program under LockSight?
# Runs each scenario of bench_locks 5 times with and without LockSight and
# reports the MEDIAN time per lock/unlock sequence, plus peak memory.
# Output: bench/results/overhead.md
cd "$(dirname "$0")" || exit 1
mkdir -p results
LIB="$PWD/../src/liblocksight.so"
[ -f "$LIB" ] || { echo "run 'make' in the LockSight folder first"; exit 1; }
gcc -O2 -pthread -o bench_locks bench_locks.c || exit 1
gcc -O2 -o maxrss maxrss.c || exit 1
REPS=${REPS:-5}

median() { sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }

# scenario  iterations
SCEN="single:2000000 nested:1000000 contended:1000000 nested4:1000000 private4:1000000 chain500:2000 realistic:400000 rwread4:1000000 rwnested4:1000000 sem4:1000000"
OUT=results/overhead.md
{
echo "| Scenario | Without LockSight (ns/iter) | With LockSight (ns/iter) | Slowdown | Peak memory without (KB) | Peak memory with (KB) |"
echo "|---|---|---|---|---|---|"
} > "$OUT"

for s in $SCEN; do
    name=${s%%:*}; it=${s##*:}
    base=(); with=(); mb=0; mw=0
    for r in $(seq "$REPS"); do
        base+=("$(./bench_locks "$name" "$it" | awk '{print $4}')")
        with+=("$(LD_PRELOAD="$LIB" LOCKSIGHT_QUIET=1 ./bench_locks "$name" "$it" 2>/dev/null | awk '{print $4}')")
    done
    b=$(printf '%s\n' "${base[@]}" | median)
    w=$(printf '%s\n' "${with[@]}" | median)
    mb=$(./maxrss ./bench_locks "$name" "$it" 2>&1 >/dev/null | awk '/MAXRSS_KB/{print $2}')
    mw=$(LD_PRELOAD="$LIB" LOCKSIGHT_QUIET=1 ./maxrss ./bench_locks "$name" "$it" 2>&1 >/dev/null | awk '/MAXRSS_KB/{print $2}')
    ratio=$(awk -v b="$b" -v w="$w" 'BEGIN{printf "%.1fx", w/b}')
    printf '| %s | %.1f | %.1f | %s | %s | %s |\n' "$name" "$b" "$w" "$ratio" "$mb" "$mw" >> "$OUT"
    echo "  $name done"
done
# Start-up cost: wall-clock time of a whole (almost empty) process, 30 runs.
startup() {      # startup <prefix-env or empty>
    local t0 t1
    t0=$(date +%s%N)
    for _ in $(seq 30); do env $1 ./bench_locks nop 1 >/dev/null 2>&1; done
    t1=$(date +%s%N)
    awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", (b-a)/30/1e6}'
}
S0=$(startup "A=1"); S1=$(startup "LD_PRELOAD=$LIB LOCKSIGHT_QUIET=1")
{
echo
echo "Start-up cost of one almost-empty process (average of 30 runs): ${S0} ms without LockSight, ${S1} ms with LockSight."
} >> "$OUT"
echo
cat "$OUT"
