# Notes for writing the report

These are facts, numbers and honest limits taken from this project's own
measurements (bench/results, 2026-10-11, i3-1005G1, 4 CPUs, WSL2, gcc 15.2).
Write the report in your own words. Every paper in References must be found
and checked by you; none are listed here on purpose.

## Mapping to the required sections

1. Abstract: problem (deadlocks depend on timing, so testing misses them),
   method (LD_PRELOAD + lock-order graph), result (24 of 28 test programs
   judged correctly vs 15 for ThreadSanitizer and 13 for Helgrind; warns
   before a real freeze), cost (about 1.8x on a realistic workload).
2. Introduction: what a deadlock is, the four conditions (mutual exclusion,
   hold and wait, no preemption, circular wait), why a lucky run proves nothing,
   goal and scope of LockSight.
3. Technical survey: dynamic lock-order analysis (the GoodLock idea), what
   ThreadSanitizer and Helgrind do, static analysis vs dynamic, deadlock
   avoidance/prevention in OS textbooks (resource ordering, Banker's algorithm),
   how LD_PRELOAD and dlsym(RTLD_NEXT) interpose library calls.
4. Findings: tables below.
5. Inference: what the numbers mean, limits, false alarms and misses.
6. Conclusion.
7. References: you find and verify them.

## How LockSight works (for the survey/method text)

- Library loaded first with LD_PRELOAD; our pthread_mutex_lock runs, records,
  then calls the real one found with dlsym(RTLD_NEXT).
- Each thread keeps a list of locks it holds. Taking B while holding A adds an
  edge A -> B (with thread, mode, call stack) to a global graph.
- A new edge that closes a cycle is a potential deadlock. Three filters remove
  impossible cycles: edges must come from different threads; a common "gate"
  lock held by both sides (exclusively on at least one side) rules the cycle
  out; adjacent links must conflict (reader/reader does not).
- Warning is printed before the real lock call, so it appears before a freeze.
- No malloc in the hot path: fixed tables, a re-entrancy guard, generation
  numbers to drop stale holds when a mutex is destroyed and its memory reused.
- Edges are indexed by (from, to) so insertion is O(1); a dynamic topological
  order (Pearce-Kelly) skips searches that cannot find a cycle; per-search and
  total search budgets bound the work.

## Findings (copy the numbers from bench/results/*.md of your own run)

Overhead per lock operation (median of 5): single 7.0x, nested 8.3x,
contended 2.8x, nested4 4.5x, private4 9.2x, chain500 13.3x, realistic 1.8x,
rwread4 1.7x, rwnested4 4.3x, sem4 1.3x. Start-up: 3.37 ms to 10.42 ms.
Peak memory: about +0.2 to +0.5 MB (chain500: +2.2 MB).

Detection, 28 programs: LockSight 24 correct, ThreadSanitizer 15, Helgrind 13.
- LockSight alone catches: self-deadlock, read-to-write lock upgrade, binary
  semaphore order inversion.
- ThreadSanitizer/Helgrind false alarms that LockSight avoids: gate lock,
  single thread both orders, mutex memory reuse, read-only locks, trylock
  (Helgrind), error-checking mutex (Helgrind), semaphore hand-off.
- All three miss: bad order in code that did not run, counting-semaphore
  deadlock, condition-variable lost wakeup.
- All three raise a false alarm on opposite orders separated by thread join.

Warning before a real freeze (8 programs that freeze after 5 s): LockSight
warned in 8 of 8; ThreadSanitizer and Helgrind in 0 of 8 (they report after
the fact, and the program was frozen).

Speed against other tools: nested4, 200k iterations: LockSight 5.0x,
ThreadSanitizer 25.9x, Helgrind 213.6x. realistic, 100k iterations:
LockSight 1.8x, ThreadSanitizer 110.2x, Helgrind 47.0x.

Real programs (9): all exit codes unchanged, no false alarms, time overhead
0 to about 36% (zstd 0.32 s to 0.43 s is the largest relative gap; git gc
9.14 s to 10.44 s). xz and sort showed 0 locks because they use their own
synchronization or none through pthread locks, which is itself worth stating.

## Honest limits (put them in Inference)

- Only runs it sees: an unexecuted path is missed, like every dynamic tool.
- Join/start ordering gives a false alarm (ordered_by_join).
- Chain500 (very deep nesting) is the worst case at 13.3x.
- TSan is faster than LockSight on lock-heavy programs with little other
  work; its advantage for LockSight is on programs with real memory work and
  on warning before a freeze. Do not claim a general speed win.
- Search budget: after 60M edges examined overall, new edges are not searched.
- Gate lock detection only for lock ids below 256.
- Not a replacement for TSan: TSan also finds data races, LockSight does not.
- Tested on Linux x86-64 glibc only; benchmark machine was WSL2 on 4 CPUs.

## How it was tested (Findings or Inference)

86 checks: 200 random mutex programs and 200 random mutex+rwlock programs
compared with an exhaustive-interleaving oracle (all verdicts matched), 100
safe and 100 shuffled many-thread programs, a 50,000-lock stress test that
must finish in 10 s, JSON output checks, command-line checks, and mutation
tests (deliberately broken versions of the library that the checks must catch).

## Suggested figures and tables

1. Lock-order graph of classic AB-BA with the cycle marked.
2. Overhead bar chart from section 1 of the benchmark.
3. Table of the 28 detection programs.
4. Time to warn vs time of freeze.
