#!/usr/bin/env bash
# check.sh - runs every test program under LockSight and checks the verdict.
#
#   WARN    LockSight MUST print a warning (a real bug, or a self-deadlock)
#   SILENT  LockSight must print NO warning (correct code that merely looks risky)
#
# Many programs have two modes:
#   --lucky   the threads happen not to overlap, so the program finishes fine
#             (the hidden-bug case: no freeze, but LockSight must still warn)
#   normal    the threads overlap and the program really freezes (exit code 124
#             from the program's watchdog). The warning must already be printed
#             BEFORE the freeze, otherwise it would be useless.
#
# At the end, 200 randomly generated programs are checked against a brute-force
# answer computed by the programs themselves (see tests/random_orders.c).
#
# Usage:  bash tests/check.sh      (or:  make check)

cd "$(dirname "$0")/.." || exit 1
LIB="$PWD/src/liblocksight.so"
PATTERN="POTENTIAL DEADLOCK|SELF-DEADLOCK"

if [ ! -f "$LIB" ]; then
    echo "src/liblocksight.so not found. Run 'make' first."
    exit 1
fi

pass=0
fail=0

report() {      # report <PASS|FAIL> <text>
    printf '  %-4s  %s\n' "$1" "$2"
    if [ "$1" = "PASS" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
}

# check <program> <mode: "" or --lucky> <WARN|SILENT> <expected exit code> [extra text that must appear in stderr]
check() {
    local prog=$1 mode=$2 expect=$3 want_code=$4 extra=$5
    local out code warns result="PASS"

    out=$(LD_PRELOAD="$LIB" timeout 60 "./tests/$prog" $mode 2>&1 >/dev/null)
    code=$?
    warns=$(printf '%s\n' "$out" | grep -c -E "$PATTERN")

    [ "$expect" = "WARN" ]   && [ "$warns" -lt 1 ] && result="FAIL"
    [ "$expect" = "SILENT" ] && [ "$warns" -ne 0 ] && result="FAIL"
    [ "$code" -ne "$want_code" ]                   && result="FAIL"
    if [ -n "$extra" ] && ! printf '%s\n' "$out" | grep -q -F -- "$extra"; then result="FAIL"; fi
    if printf '%s\n' "$out" | grep -q "table limits reached"; then result="FAIL"; fi

    report "$result" "$(printf '%-24s %-9s warnings=%-2s exit=%-3s (wanted: %s, exit %s)' \
        "$prog" "${mode:-(normal)}" "$warns" "$code" "$expect" "$want_code")"
}

echo "LockSight check"
echo "---------------"
echo "Real deadlock bugs (must warn, also when the program does not freeze):"
for p in classic_deadlock three_lock_cycle bank_transfer dining_philosophers partial_gate; do
    check "$p" "--lucky" WARN 0
    check "$p" ""        WARN 124      # 124 = the program froze (watchdog)
done
check self_deadlock ""  WARN 124 "SELF-DEADLOCK"
check big_ring      ""  WARN 0   "more)"
check big_ring      "100" WARN 0 "more)"
check worker_pool_outlier "" WARN 0

echo "Correct code that only LOOKS risky (must stay silent):"
check safe_ordered          "--lucky" SILENT 0
check safe_ordered          ""        SILENT 0
check single_thread_both_orders "" SILENT 0
check gate_lock             "--lucky" SILENT 0
check gate_lock             ""        SILENT 0
check trylock_case          "--lucky" SILENT 0
check trylock_case          ""        SILENT 0
check recursive_ok          ""        SILENT 0
check errorcheck_ok         ""        SILENT 0
check mutex_reuse           ""        SILENT 0

echo "Stress test (memory reuse, recycling of LockSight's tables):"
check many_locks "" SILENT 0 "(0 still tracked)"

echo "Read-write locks (read = shared, write = exclusive):"
check rwlock_mixed        "--lucky" WARN   0 "rwlock"
check rwlock_mixed        ""        WARN 124
check rwlock_rw_cycle     "--lucky" WARN   0 "(read)"
check rwlock_rw_cycle     ""        WARN 124
check rwlock_gate         "--lucky --shared" WARN 0
check rwlock_gate         "--shared"         WARN 124
check rwlock_upgrade      ""        WARN 124 "SELF-DEADLOCK"
echo "Read-write locks that only LOOK risky (must stay silent):"
check rwlock_readers_ok   "--lucky" SILENT 0
check rwlock_readers_ok   ""        SILENT 0
check rwlock_oneway_ok    "--lucky" SILENT 0
check rwlock_oneway_ok    ""        SILENT 0
check rwlock_gate         "--lucky" SILENT 0
check rwlock_gate         ""        SILENT 0

echo "Binary semaphores (sem_init(.., 1) = a lock) and hand-offs:"
check sem_binary_deadlock "--lucky" WARN   0
check sem_binary_deadlock ""        WARN 124
check sem_binary_ok       ""        SILENT 0
check sem_handoff_ok      ""        SILENT 0 "note: semaphore"
check mutex_handoff_ok    ""        SILENT 0
out=$(LD_PRELOAD="$LIB" timeout 30 ./tests/sem_binary_ok 2>&1 >/dev/null)
if [[ "$out" != *"note:"* ]]; then report PASS "sem_binary_ok: no note (the posts come from the thread that waited)"
else report FAIL "sem_binary_ok: an unexpected note was printed"; fi

echo "Random programs checked against brute force (seeds 1-200):"
tmp=$(mktemp)
n_dead=0; n_safe=0; bad=""
for seed in $(seq 1 200); do
    out=$(LD_PRELOAD="$LIB" timeout 10 ./tests/random_orders "$seed" 2>"$tmp")
    expect=$(printf '%s\n' "$out" | sed -n 's/^EXPECT: //p')
    warns=$(grep -c -E "$PATTERN" "$tmp")
    if [ "$expect" = "DEADLOCK" ]; then
        n_dead=$((n_dead + 1)); [ "$warns" -ge 1 ] || bad="$bad $seed"
    else
        n_safe=$((n_safe + 1)); [ "$warns" -eq 0 ] || bad="$bad $seed"
    fi
done
rm -f "$tmp"
if [ -z "$bad" ]; then
    report PASS "random_orders: all 200 verdicts match ($n_dead with a possible deadlock, $n_safe safe)"
else
    report FAIL "random_orders: LockSight and brute force disagree on seeds:$bad"
fi

echo "Random mutex + read-write lock programs checked by exhaustive search (seeds 1-200):"
tmp=$(mktemp)
n_dead=0; n_safe=0; bad=""
for seed in $(seq 1 200); do
    out=$(LD_PRELOAD="$LIB" timeout 10 ./tests/random_rw "$seed" 2>"$tmp")
    expect=$(printf '%s\n' "$out" | sed -n 's/^EXPECT: //p')
    warns=$(grep -c -E "$PATTERN" "$tmp")
    if [ "$expect" = "DEADLOCK" ]; then
        n_dead=$((n_dead + 1)); [ "$warns" -ge 1 ] || bad="$bad $seed"
    else
        n_safe=$((n_safe + 1)); [ "$warns" -eq 0 ] || bad="$bad $seed"
    fi
done
rm -f "$tmp"
if [ -z "$bad" ]; then
    report PASS "random_rw: all 200 verdicts match ($n_dead with a possible deadlock, $n_safe safe)"
else
    report FAIL "random_rw: LockSight and the exhaustive search disagree on seeds:$bad"
fi


echo "Many locks (the tables and the speed of adding edges):"
SECONDS=0
check hub_many "" SILENT 0 "NO PROBLEMS FOUND"
if [ "$SECONDS" -le 10 ]; then
    report PASS "hub_many: 50000 locks under one hub lock took ${SECONDS}s (must be 10s or less)"
else
    report FAIL "hub_many took ${SECONDS}s: adding edges has become slow"
fi
tmp=$(mktemp)
bad=""
for seed in $(seq 1 100); do
    LD_PRELOAD="$LIB" timeout 30 ./tests/random_many "$seed" 0 >/dev/null 2>"$tmp"
    [ "$(grep -c -E "$PATTERN" "$tmp")" -eq 0 ] || bad="$bad $seed"
done
rm -f "$tmp"
if [ -z "$bad" ]; then
    report PASS "random_many: 100 big programs with a global lock order, no warnings"
else
    report FAIL "random_many: false warnings on seeds:$bad"
fi
tmp=$(mktemp)
miss=""
for seed in $(seq 1 100); do
    LD_PRELOAD="$LIB" timeout 30 ./tests/random_many "$seed" 100 >/dev/null 2>"$tmp"
    [ "$(grep -c -E "$PATTERN" "$tmp")" -ge 1 ] || miss="$miss $seed"
done
rm -f "$tmp"
# With every chain shuffled, almost every program has a possible deadlock; a few
# small ones may be safe by chance, so allow a handful of silent seeds.
nmiss=$(echo $miss | wc -w)
if [ "$nmiss" -le 10 ]; then
    report PASS "random_many: shuffled chains: $((100 - nmiss))/100 programs warn (fully shuffled programs nearly always can deadlock)"
else
    report FAIL "random_many: only $((100 - nmiss))/100 shuffled programs gave a warning"
fi

echo "Command-line tool (./locksight):"

# cli_case <label> <wanted exit> <stderr must contain> <stderr must NOT contain> <stdout must contain> -- command...
# Use "" for "no condition".
cli_case() {
    local label=$1 want=$2 err_has=$3 err_not=$4 out_has=$5
    shift 6
    local ef out err code result="PASS"
    ef=$(mktemp)
    out=$("$@" 2>"$ef"); code=$?
    err=$(cat "$ef"); rm -f "$ef"
    [ "$code" -ne "$want" ] && result="FAIL"
    if [ -n "$err_has" ] && [[ "$err" != *"$err_has"* ]]; then result="FAIL"; fi
    if [ -n "$err_not" ] && [[ "$err" == *"$err_not"* ]]; then result="FAIL"; fi
    if [ -n "$out_has" ] && [[ "$out" != *"$out_has"* ]]; then result="FAIL"; fi
    report "$result" "$(printf '%-55s exit=%-3s (wanted %s)' "$label" "$code" "$want")"
}

cli_case "run: bug found, exit 3, program output kept"      3 "POTENTIAL DEADLOCK" "" "final balances" -- ./locksight run ./tests/bank_transfer --lucky
cli_case "run: correct program, exit 0, summary says so"    0 "NO PROBLEMS FOUND" "POTENTIAL" "counter = 2000" -- ./locksight run ./tests/safe_ordered
cli_case "run: the program's own arguments are passed on"   3 "more)" "" "" -- ./locksight run ./tests/big_ring 50
cli_case "--quiet: warning, but no summary"                 3 "POTENTIAL DEADLOCK" "Summary" "" -- ./locksight run --quiet ./tests/bank_transfer --lucky
cli_case "--quiet: correct program prints nothing"          0 "" "LockSight" "counter = 2000" -- ./locksight run --quiet ./tests/safe_ordered
cli_case "the program's own exit status is kept (7)"        7 "" "" "" -- ./locksight run sh -c 'exit 7'
cli_case "program not found -> 127"                         127 "cannot find program" "" "" -- ./locksight run ./no_such_program
cli_case "no arguments -> usage, exit 2"                    2 "Usage" "" "" -- ./locksight
cli_case "unknown option -> exit 2"                         2 "unknown option" "" "" -- ./locksight run --bogus ./tests/safe_ordered
cli_case "--help"                                           0 "" "" "Usage:" -- ./locksight --help
cli_case "--version"                                        0 "" "" "LockSight" -- ./locksight --version

SECONDS=0
cli_case "--strict: stops a freezing program, exit 3"       3 "--strict: stopping" "" "" -- ./locksight run --strict ./tests/classic_deadlock
if [ "$SECONDS" -le 3 ]; then
    report PASS "--strict: stopped within ${SECONDS}s (the program alone freezes for 5s)"
else
    report FAIL "--strict was too slow (${SECONDS}s)"
fi
cli_case "--strict: a correct program is not affected"      0 "NO PROBLEMS FOUND" "" "" -- ./locksight run --strict ./tests/safe_ordered
cli_case "frozen program: exit 3 and an honest message"     3 "did not exit normally" "" "" -- ./locksight run ./tests/self_deadlock

logf=$(mktemp)
cli_case "--log: terminal output is colored when forced"    3 $'\033[' "" "" -- env LOCKSIGHT_COLOR=always ./locksight run --log "$logf" ./tests/bank_transfer --lucky
if grep -q "POTENTIAL DEADLOCK" "$logf" && grep -q "Summary" "$logf" && ! grep -q $'\033' "$logf"; then
    report PASS "--log: the file has the warning and the summary, no colors"
else
    report FAIL "--log: the file content is wrong"
fi
rm -f "$logf"
cli_case "no colors when the output is not a terminal"      3 "POTENTIAL" $'\033' "" -- ./locksight run ./tests/bank_transfer --lucky

if command -v script >/dev/null 2>&1; then
    tty_out=$(script -qec "./locksight run ./tests/bank_transfer --lucky" /dev/null 2>&1)
    if [ -z "$tty_out" ]; then
        echo "  SKIP  terminal color tests ('script' could not open a terminal here)"
    else
        if [[ "$tty_out" == *$'\033['* ]]; then report PASS "colors are on in a real terminal"
        else report FAIL "no colors in a real terminal"; fi
        tty_out=$(NO_COLOR=1 script -qec "./locksight run ./tests/bank_transfer --lucky" /dev/null 2>&1)
        if [[ "$tty_out" != *$'\033['* ]]; then report PASS "NO_COLOR=1 turns the colors off"
        else report FAIL "NO_COLOR=1 was ignored"; fi
    fi
else
    echo "  SKIP  terminal color tests (the 'script' command is not installed)"
fi

echo "JSON output (--json FILE, one JSON object per line):"

# json_ok FILE -> every line is valid JSON (python3 or jq if present, else a structural check)
json_ok() {
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import json,sys
for l in open(sys.argv[1]): json.loads(l)' "$1" 2>/dev/null
    elif command -v jq >/dev/null 2>&1; then
        jq -e . "$1" >/dev/null 2>&1
    else
        ! grep -v -q -E '^\{.*\}$' "$1"
    fi
}
if command -v python3 >/dev/null 2>&1; then jmode="validated with python3"
elif command -v jq >/dev/null 2>&1; then jmode="validated with jq"
else jmode="structure only: install python3 or jq for full validation"; fi

jf=$(mktemp)
cli_case "--json: bug found, exit 3, text report still printed" 3 "POTENTIAL DEADLOCK" "" "" -- ./locksight run --json "$jf" ./tests/bank_transfer --lucky
if json_ok "$jf" && grep -q '"event":"potential_deadlock"' "$jf" && grep -q '"event":"summary"' "$jf" \
   && grep -q '"potential_deadlocks":1' "$jf" && grep -q '"result":"problems_found"' "$jf"; then
    report PASS "--json: valid, has potential_deadlock + summary events ($jmode)"
else report FAIL "--json: file is invalid or incomplete"; fi
if grep -q '"file":"bank_transfer.c"' "$jf" && grep -q '"function":"transfer"' "$jf" && grep -q -E '"line":[0-9]+' "$jf" \
   && grep -q '"mode":"exclusive"' "$jf" && grep -q '"kind":"mutex"' "$jf" && grep -q '"thread":' "$jf"; then
    report PASS "--json: locks, threads, modes and file/line/function are structured fields"
else report FAIL "--json: structured fields are missing"; fi

cli_case "--json with --quiet: no text summary on screen" 3 "POTENTIAL DEADLOCK" "Summary" "" -- ./locksight run --quiet --json "$jf" ./tests/bank_transfer --lucky
if json_ok "$jf" && grep -q '"event":"summary"' "$jf"; then report PASS "--json with --quiet: the file still gets the summary event"
else report FAIL "--json with --quiet: summary event missing"; fi

LOCKSIGHT_COLOR=always ./locksight run --json "$jf" ./tests/bank_transfer --lucky >/dev/null 2>&1
if json_ok "$jf" && ! grep -q $'\033' "$jf" && ! grep -q $'\001' "$jf"; then report PASS "--json: no color codes or markers in the file"
else report FAIL "--json: the file contains color codes"; fi

./locksight run --json "$jf" ./tests/safe_ordered >/dev/null 2>&1
if json_ok "$jf" && [ "$(grep -c '"event"' "$jf")" -eq 1 ] && grep -q '"result":"no_problems"' "$jf"; then
    report PASS "--json: a correct program gives only a summary with no_problems"
else report FAIL "--json: correct program output is wrong"; fi

./locksight run --json "$jf" ./tests/self_deadlock >/dev/null 2>&1
if json_ok "$jf" && grep -q '"event":"self_deadlock"' "$jf" && grep -q '"requested_at"' "$jf"; then
    report PASS "--json: self_deadlock event is written at once (the program froze, no summary)"
else report FAIL "--json: self_deadlock event missing"; fi

./locksight run --json "$jf" ./tests/rwlock_rw_cycle --lucky >/dev/null 2>&1
if json_ok "$jf" && grep -q '"mode":"shared"' "$jf" && grep -q '"kind":"rwlock"' "$jf"; then
    report PASS "--json: rwlock locks and shared/exclusive modes are reported"
else report FAIL "--json: rwlock modes missing"; fi

./locksight run --json "$jf" ./tests/sem_handoff_ok >/dev/null 2>&1
if json_ok "$jf" && grep -q '"note":"semaphore_signaling"' "$jf"; then
    report PASS "--json: the semaphore note is an event"
else report FAIL "--json: semaphore note missing"; fi

# a program built without -g has no line numbers: they must be null, never 0
wd0=$(mktemp -d)
if gcc -O0 -pthread -o "$wd0/nodebug" tests/classic_deadlock.c 2>/dev/null; then
    LD_PRELOAD="$LIB" LOCKSIGHT_JSON="$jf" "$wd0/nodebug" --lucky >/dev/null 2>&1
    if json_ok "$jf" && grep -q '"event":"potential_deadlock"' "$jf" && ! grep -q '"line":0' "$jf"; then
        report PASS "--json: a program without debug info still gives valid JSON (no line 0)"
    else report FAIL "--json: no-debug-info output is wrong"; fi
else report FAIL "--json: could not build the no-debug-info test"; fi
rm -rf "$wd0"

# odd file names and an event too big for the buffer must still give valid JSON
wd=$(mktemp -d)
mkdir -p "$wd/dir"
cp tests/classic_deadlock.c "$wd/dir/a\"b.c"; cp tests/common.h "$wd/dir/"
if gcc -g -O0 -pthread -o "$wd/q" "$wd/dir/a\"b.c" 2>/dev/null; then
    LD_PRELOAD="$LIB" LOCKSIGHT_JSON="$jf" "$wd/q" --lucky >/dev/null 2>&1
    if json_ok "$jf" && grep -q -F '"file":"a\"b.c"' "$jf"; then report PASS "--json: a quote in a file name is escaped correctly"
    else report FAIL "--json: escaping is broken"; fi
else report FAIL "--json: could not build the odd-file-name test"; fi
if gcc -shared -fPIC -O2 -g -pthread -DJSON_SIZE=1800 -o "$wd/small.so" src/liblocksight.c -ldl 2>/dev/null; then
    LD_PRELOAD="$wd/small.so" LOCKSIGHT_JSON="$jf" ./tests/big_ring 50 >/dev/null 2>&1
    if json_ok "$jf" && grep -q '"details_truncated":true' "$jf" && grep -q '"event":"summary"' "$jf"; then
        report PASS "--json: an event that does not fit is replaced by a short valid line"
    else report FAIL "--json: overflow handling is broken"; fi
else report FAIL "--json: could not build the small-buffer test"; fi
rm -rf "$wd" "$jf"

cli_case "--json without a file name -> exit 2"             2 "needs a file name" "" "" -- ./locksight run --json
cli_case "--json to an unwritable place -> exit 2"          2 "cannot write to JSON file" "" "" -- ./locksight run --json /no/such/dir/x.jsonl ./tests/safe_ordered

inst=$(mktemp -d)
make -s install PREFIX="$inst" >/dev/null 2>&1
cli_case "installed command works from another folder"      3 "POTENTIAL" "" "" -- bash -c "cd / && '$inst/bin/locksight' run '$PWD/tests/bank_transfer' --lucky"
rm -rf "$inst"

echo "---------------"
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
