# LockSight limits

Default build: 65,536 locks, 262,144 lock-order edges. Edges are indexed by
(from, to), so adding one is O(1). Typical servers fit comfortably.

Bigger build (about 280 MB reserved, 40-170 MB touched at 100-200k locks):

    make lib LIMITS="-DMAX_LOCKS=1000000 -DMAX_EDGES=1000000"

When a limit is hit LockSight says so in its summary and stops tracking new
locks; it never crashes the program.

Known limits:
- A hub lock held while thousands of other locks are taken in both orders
  can make cycle search slow (superlinear). After a global search budget the
  summary says "the cycle search gave up" and new edges are no longer checked.
- Gate-lock detection uses ids below 256 only.
- Tests: `make check` includes hub_many (50,000 locks, must finish in 10 s)
  and random_many (many threads, many locks, compared with a known answer).
