# LockSight

Finds potential deadlocks in C/C++ pthread programs BEFORE they happen.
No source changes, no recompiling: it watches the program's lock calls through
LD_PRELOAD and keeps a graph of "lock A was held while taking lock B". A loop
in that graph is a possible deadlock, reported even if this run got lucky.

## Build and run

    cd ~/locksight
    make              # builds liblocksight.so and the tests
    make check        # runs all checks (about 2 minutes)
    make install      # optional: puts `locksight` on your PATH

    ./locksight run ./your_program [its arguments]

Exit status: the program's own status; 3 if LockSight reported a problem
(a possible deadlock, a self-deadlock, or a freeze it stopped); 2 for wrong
usage; 127 if the program was not found.

## Options

    --quiet          warnings only, no summary
    --strict         stop a program that really freezes, exit 3
    --log FILE       copy the report (no colors) to FILE
    --json FILE      one JSON object per line, for scripts (docs/JSON_FORMAT.md)
    --help, --version

Environment: LOCKSIGHT_QUIET, _STRICT, _LOG, _JSON, _STATUS, _COLOR; NO_COLOR=1
turns colors off.

## What it covers

- pthread mutexes (normal, recursive, error-checking), including C++ std::mutex
- pthread read-write locks (shared readers never count as a conflict)
- binary semaphores (sem_init with value 1); a post from a thread that never
  waited is treated as signaling and that semaphore is dropped from tracking
- trylock, gate locks, single-thread use of both orders, memory reuse of a
  destroyed mutex: all handled without false alarms

## Limits (see docs/SCALING.md)

64k locks and 256k edges by default; bigger build:
`make lib LIMITS="-DMAX_LOCKS=1000000 -DMAX_EDGES=1000000"`.

Not detected: lock orders on code paths the run never executes, counting
semaphores used as resources, condition-variable lost wakeups, deadlocks
through non-pthread locks (spinlocks, custom locks). Known false alarm:
orders separated by thread start/join (a happens-before relation LockSight
does not track). A cycle search budget exists; if it runs out the summary says
"the cycle search gave up".

## Layout

    src/liblocksight.c   the library (about 2,300 lines of C, no allocation at run time)
    locksight            the launcher script
    tests/               test programs and check.sh
    bench/               overhead, comparison with ThreadSanitizer/Helgrind, real programs
    docs/                JSON_FORMAT.md, SCALING.md, REPORT_NOTES.md
