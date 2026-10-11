# What LockSight catches and what it misses

Every row below was tested with a program in `tests/` or `bench/extra/`
(see results/summary.md for the measured verdicts of all three tools).
Phase 6 added read-write locks and binary semaphores.

| Kind of problem | LockSight | Why |
|---|---|---|
| Two mutexes locked in opposite orders by different threads | Caught, also when the run does not freeze | lock-order graph cycle |
| Cycle over 3 or more mutexes (dining philosophers, big rings) | Caught | cycle search over the whole graph |
| C++ `std::mutex` / `std::lock_guard` | Caught | they call `pthread_mutex_lock` underneath |
| Same thread locks a normal mutex twice | Caught (SELF-DEADLOCK) | held-lock list of the thread |
| Opposite orders, but both under a common outer lock | Silent (correct) | gate-lock filter |
| One thread using both orders | Silent (correct) | distinct-threads filter |
| Recursive / error-checking mutex locked twice | Silent (correct) | mutex type is read |
| `pthread_mutex_trylock` as the second lock | Silent (correct) | trylock cannot block, so it adds no edge |
| Reused memory of a destroyed mutex | Silent (correct) | init/destroy wrappers forget the old mutex |
| `pthread_rwlock_t` inversion (write locks) | Caught | rwlocks are tracked, with a shared/exclusive mode |
| Read lock held, write lock requested on the other lock (both threads) | Caught | a reader blocks a writer |
| Only read locks, opposite orders | Silent (correct) | readers never block each other |
| Mutex and rwlock mixed in opposite orders | Caught | one graph for all lock kinds |
| Read lock upgraded to a write lock by the same thread | Caught (SELF-DEADLOCK) | the writer waits for all readers, including itself |
| Opposite orders under an outer rwlock taken for WRITING | Silent (correct) | an exclusive outer lock is a gate |
| Opposite orders under an outer rwlock taken for READING | Caught | a shared outer lock does not keep the threads apart |
| Binary semaphores (`sem_init(&s, 0, 1)`) in opposite orders | Caught | treated as locks owned by the thread that waited |
| Semaphore posted by a thread that never waited on it | Note, then not tracked | it is a signal, not a lock |
| Lock handed from one thread to another (locked in A, unlocked in B) | Silent (correct) | holds carry a generation number and stale ones are ignored |
| Bad order in code that this run never executes | MISSED | dynamic analysis sees executed paths only |
| Semaphore cycles where the semaphores start at 0 (signaling) | MISSED | not locks: nobody "owns" them, so there is no lock order |
| Condition variable lost wake-up | MISSED | not a lock cycle |
| `pthread_join` cycles | MISSED | not a mutex |
| Non-pthread locks (Go, Java, Python `threading`, spinlocks, own atomics) | MISSED | not `pthread_mutex_*` calls |
| Statically linked or setuid programs | Not working | `LD_PRELOAD` is ignored |

## False alarms (LockSight warns although no deadlock can happen)

1. **Orders separated by thread start/join** (`bench/extra/ordered_by_join.c`):
   thread 2 only starts after thread 1 was joined, so they never overlap, but
   the lock orders are opposite. LockSight does not model thread start/join,
   so it warns. ThreadSanitizer and Helgrind do the same.
(An earlier version also reported a bogus SELF-DEADLOCK for a lock locked by
one thread and unlocked by another. Phase 6 fixed that: `tests/mutex_handoff_ok.c`.)

## Hard limits of the implementation

- Gate-lock tracking uses a 256-lock bitset per edge: locks with an id above
  255 are not tracked as gates (this can only cause an extra warning, never hide one).
- Per edge pair at most 4 different threads are remembered.
- The cycle search stops after 200000 steps or depth 256 (huge graphs).
- A normal mutex that a thread unlocks while another thread owns it is
  not diagnosed (it is just treated as a hand-off).
- Read-write locks assume glibc's default (readers are not held back by a
  WAITING writer). A program that asks for writer preference can deadlock in
  ways this model does not see.
- A semaphore is only a lock for LockSight if `sem_init(&s, 0, 1)` was called
  in the traced program. Semaphores from `sem_open`, process-shared ones
  (`pshared != 0`) and ones with any other starting value are not tracked, and
  waiting twice on a binary semaphore is not reported (the second wait may be
  waiting for another thread's `sem_post`).
- `pthread_rwlock_clockrdlock`/`clockwrlock` and `pthread_mutex_clocklock` are
  not intercepted; a lock taken that way is simply not known as held.
- A warning means "this order CAN deadlock under some schedule", not "it will".
