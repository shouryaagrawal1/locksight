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

inst=$(mktemp -d)
make -s install PREFIX="$inst" >/dev/null 2>&1
cli_case "installed command works from another folder"      3 "POTENTIAL" "" "" -- bash -c "cd / && '$inst/bin/locksight' run '$PWD/tests/bank_transfer' --lucky"
rm -rf "$inst"

echo "---------------"
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
