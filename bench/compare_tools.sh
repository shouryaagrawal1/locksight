#!/usr/bin/env bash
# compare_tools.sh - LockSight vs ThreadSanitizer vs Valgrind Helgrind.
#
# For every test program it asks each tool: "do you report a lock problem?"
# and checks the answer against the truth (BUG = the code can deadlock,
# OK = the code is correct). It also checks whether the warning appears
# BEFORE a real freeze, and measures speed.
#
# Needs: gcc, valgrind (sudo apt install valgrind) and libtsan (comes with gcc;
#        if "gcc -fsanitize=thread" fails: sudo apt install libtsan2 / libtsan0).
# Output: bench/results/detection.md  bench/results/speed.md
cd "$(dirname "$0")" || exit 1
ROOT=$PWD/..
LIB="$ROOT/src/liblocksight.so"
[ -f "$LIB" ] || { echo "run 'make' in the LockSight folder first"; exit 1; }
mkdir -p results build

HAVE_VG=1; command -v valgrind >/dev/null || { HAVE_VG=0; echo "NOTE: valgrind not installed -> Helgrind column will say n/a"; }
HAVE_TSAN=1
echo 'int main(void){return 0;}' > build/t.c
gcc -fsanitize=thread -o build/t build/t.c 2>/dev/null && ./build/t 2>/dev/null || { HAVE_TSAN=0; echo "NOTE: ThreadSanitizer not usable -> TSan column will say n/a"; }

# ---- build the programs: plain (for LockSight / Helgrind) and TSan -------------
SRC_BUGS="classic_deadlock three_lock_cycle bank_transfer dining_philosophers partial_gate self_deadlock rwlock_mixed rwlock_rw_cycle rwlock_upgrade sem_binary_deadlock"
SRC_OK="safe_ordered gate_lock trylock_case single_thread_both_orders recursive_ok errorcheck_ok mutex_reuse rwlock_readers_ok rwlock_oneway_ok sem_binary_ok sem_handoff_ok mutex_handoff_ok"
for p in $SRC_BUGS $SRC_OK; do
    gcc -g -O0 -pthread -o build/$p "$ROOT/tests/$p.c" || exit 1
    [ $HAVE_TSAN = 1 ] && gcc -g -O0 -pthread -fsanitize=thread -o build/$p.tsan "$ROOT/tests/$p.c"
done
for p in rwlock_inversion unexecuted_path semaphore_deadlock condvar_lost_wakeup ordered_by_join; do
    gcc -g -O0 -pthread -o build/$p extra/$p.c || exit 1
    [ $HAVE_TSAN = 1 ] && gcc -g -O0 -pthread -fsanitize=thread -o build/$p.tsan extra/$p.c
done
g++ -g -O0 -pthread -o build/cpp_mutex extra/cpp_mutex.cpp || exit 1
[ $HAVE_TSAN = 1 ] && g++ -g -O0 -pthread -fsanitize=thread -o build/cpp_mutex.tsan extra/cpp_mutex.cpp

# ---- run one tool on one program ------------------------------------------------
# run_tool <tool> <program> <mode>   -> writes build/out.txt (stdout+stderr in order),
#                                       sets T_REPORT (1/0) and T_BEFORE (1 = report came before FROZEN)
run_tool() {
    local tool=$1 prog=$2 mode=$3 pat
    case $tool in
        ls)   LD_PRELOAD="$LIB" LOCKSIGHT_QUIET=1 timeout 60 ./build/$prog $mode >build/out.txt 2>&1 ;;
        tsan) TSAN_OPTIONS="report_signal_unsafe=0" timeout 60 ./build/$prog.tsan $mode >build/out.txt 2>&1 ;;
        hg)   timeout 120 valgrind --tool=helgrind -q ./build/$prog $mode >build/out.txt 2>&1 ;;
    esac
    case $tool in
        ls)   pat="POTENTIAL DEADLOCK|SELF-DEADLOCK" ;;
        tsan) pat="lock-order-inversion|double lock of a mutex" ;;
        hg)   pat="lock order .* violated|Attempt to re-lock|lock order" ;;
    esac
    T_REPORT=0; T_BEFORE=0
    local rl fl
    rl=$(grep -n -m1 -E "$pat" build/out.txt | cut -d: -f1)
    fl=$(grep -n -m1 "FROZEN" build/out.txt | cut -d: -f1)
    [ -n "$rl" ] && T_REPORT=1
    if [ -n "$rl" ] && { [ -z "$fl" ] || [ "$rl" -lt "$fl" ]; }; then T_BEFORE=1; fi
}

verdict() {      # verdict <truth BUG|OK> <reported 0/1> -> text
    if [ "$1" = BUG ]; then [ "$2" = 1 ] && echo "caught" || echo "MISSED"
    else                    [ "$2" = 1 ] && echo "FALSE ALARM" || echo "silent (correct)"; fi
}
cell() {         # cell <tool> <truth> <prog> <mode>
    local tool=$1 truth=$2 prog=$3 mode=$4
    if [ "$tool" = hg ] && [ $HAVE_VG = 0 ]; then echo "n/a"; return; fi
    if [ "$tool" = tsan ] && [ $HAVE_TSAN = 0 ]; then echo "n/a"; return; fi
    run_tool "$tool" "$prog" "$mode"
    verdict "$truth" "$T_REPORT"
}

# ---- Table 1: detection (the program does NOT freeze in this run: "lucky" mode) ---
# prog | mode | truth | description
CASES="
classic_deadlock|--lucky|BUG|two locks, opposite order
three_lock_cycle|--lucky|BUG|cycle over three locks
bank_transfer|--lucky|BUG|transfer(a,b) vs transfer(b,a)
dining_philosophers|--lucky|BUG|5 philosophers, 5 forks
partial_gate|--lucky|BUG|common lock only for ONE of the two orders
self_deadlock||BUG|same thread locks the same mutex twice
safe_ordered|--lucky|OK|all threads lock in the same order
gate_lock|--lucky|OK|opposite orders, but both hold a common outer lock
single_thread_both_orders||OK|one thread uses both orders, never at once
trylock_case|--lucky|OK|opposite order, but the 2nd lock uses trylock
recursive_ok||OK|recursive mutex locked twice
errorcheck_ok||OK|error-checking mutex, relock handled
mutex_reuse||OK|memory of a destroyed mutex reused for a new one
cpp_mutex|--lucky|BUG|C++ std::mutex inversion
rwlock_inversion|--lucky|BUG|two pthread_rwlock_t, opposite order
unexecuted_path||BUG|bad order exists only in a branch not executed
semaphore_deadlock||BUG|sem_wait cycle (no mutex at all)
condvar_lost_wakeup||BUG|signal sent before wait (no lock cycle)
ordered_by_join||OK|opposite orders, but thread 2 starts only after thread 1 was joined
mutex_handoff_ok||OK|lock taken in one thread, released in another
rwlock_mixed|--lucky|BUG|mutex + rwlock (write) in opposite orders
rwlock_rw_cycle|--lucky|BUG|read-lock A then write-lock B, and the reverse
rwlock_upgrade||BUG|read lock upgraded to a write lock by the same thread
rwlock_readers_ok|--lucky|OK|only read locks, opposite orders
rwlock_oneway_ok|--lucky|OK|read A+read B vs write B+read A (no loop)
sem_binary_deadlock|--lucky|BUG|two binary semaphores (value 1) in opposite orders
sem_binary_ok||OK|binary semaphores used in one order
sem_handoff_ok||OK|semaphore posted by a thread that never waited (signaling; also one thread uses two mutexes in both orders, which TSan and Helgrind flag on their own)
"
D=results/detection.md
{
echo "| Program | What it is | Truth | LockSight | ThreadSanitizer | Helgrind |"
echo "|---|---|---|---|---|---|"
} > $D
ls_ok=0; ts_ok=0; hg_ok=0; n=0
while IFS='|' read -r prog mode truth desc; do
    [ -z "$prog" ] && continue
    a=$(cell ls   "$truth" "$prog" "$mode")
    b=$(cell tsan "$truth" "$prog" "$mode")
    c=$(cell hg   "$truth" "$prog" "$mode")
    echo "| $prog ${mode:+($mode)} | $desc | $truth | $a | $b | $c |" >> $D
    n=$((n+1))
    case $a in caught|silent*) ls_ok=$((ls_ok+1));; esac
    case $b in caught|silent*) ts_ok=$((ts_ok+1));; esac
    case $c in caught|silent*) hg_ok=$((hg_ok+1));; esac
    echo "  detection: $prog done"
done <<< "$CASES"
echo >> $D
echo "Correct verdicts out of $n programs: LockSight $ls_ok, ThreadSanitizer $ts_ok (n/a if not installed), Helgrind $hg_ok." >> $D

# ---- Table 2: warning BEFORE the freeze? (normal mode: the program really hangs) ---
F=results/before_freeze.md
{
echo "| Program (really freezes after 5 s) | LockSight warns before the freeze | ThreadSanitizer warns before the freeze | Helgrind warns before the freeze |"
echo "|---|---|---|---|"
} > $F
for prog in classic_deadlock three_lock_cycle bank_transfer dining_philosophers partial_gate rwlock_mixed rwlock_rw_cycle sem_binary_deadlock; do
    row="| $prog |"
    for tool in ls tsan hg; do
        if [ "$tool" = hg ] && [ $HAVE_VG = 0 ]; then row="$row n/a |"; continue; fi
        if [ "$tool" = tsan ] && [ $HAVE_TSAN = 0 ]; then row="$row n/a |"; continue; fi
        run_tool $tool $prog ""
        froze=$(grep -c FROZEN build/out.txt)
        if [ $T_BEFORE = 1 ]; then row="$row yes |"
        elif [ $T_REPORT = 1 ]; then row="$row only after |"
        else row="$row no warning (froze: $froze) |"; fi
    done
    echo "$row" >> $F
    echo "  before-freeze: $prog done"
done

# ---- Table 3: speed on a lock-heavy program ---------------------------------------
gcc -O2 -pthread -o build/bl bench_locks.c
[ $HAVE_TSAN = 1 ] && gcc -O2 -pthread -fsanitize=thread -o build/bl.tsan bench_locks.c
gcc -O0 -g -pthread -o build/bl_g bench_locks.c      # Helgrind prefers unoptimised, symbols
S=results/speed.md
{
echo "| Scenario | Plain | LockSight | ThreadSanitizer | Helgrind |"
echo "|---|---|---|---|---|"
} > $S
ms() { awk '{print $3}'; }      # bench_locks prints: scenario iters total_ms ns/iter
for spec in nested4:200000 realistic:100000; do
    sc=${spec%%:*}; it=${spec##*:}
    p=$(./build/bl $sc $it | ms)
    l=$(LD_PRELOAD="$LIB" LOCKSIGHT_QUIET=1 ./build/bl $sc $it 2>/dev/null | ms)
    if [ $HAVE_TSAN = 1 ]; then t=$(./build/bl.tsan $sc $it 2>/dev/null | ms); else t=n/a; fi
    if [ $HAVE_VG = 1 ]; then h=$(valgrind --tool=helgrind -q ./build/bl_g $sc $it 2>/dev/null | ms); else h=n/a; fi
    fmt() { awk -v x="$1" -v p="$2" 'BEGIN{ if (x=="n/a"||x=="") print "n/a"; else printf "%.0f ms (%.1fx)", x, x/p }'; }
    echo "| $sc, $it iterations | $(fmt $p $p) | $(fmt $l $p) | $(fmt $t $p) | $(fmt $h $p) |" >> $S
    echo "  speed: $sc done"
done
echo
echo "### Detection"; cat $D; echo; echo "### Warning before the freeze"; cat $F; echo; echo "### Speed (total run time, slowdown vs plain in brackets)"; cat $S
