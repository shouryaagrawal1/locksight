# LockSight measurements

Machine: Intel(R) Core(TM) i3-1005G1 CPU @ 1.20GHz, 4 logical CPUs, Linux 6.18.33.2-microsoft-standard-WSL2, gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0
Valgrind: valgrind-3.26.0
Date: 2026-10-11

## 1. Overhead (median of 5 runs)

| Scenario | Without LockSight (ns/iter) | With LockSight (ns/iter) | Slowdown | Peak memory without (KB) | Peak memory with (KB) |
|---|---|---|---|---|---|
| single | 28.7 | 200.8 | 7.0x | 1968 | 2208 |
| nested | 39.5 | 327.2 | 8.3x | 1904 | 2248 |
| contended | 147.2 | 414.7 | 2.8x | 2096 | 2376 |
| nested4 | 199.2 | 891.4 | 4.5x | 2096 | 2464 |
| private4 | 58.4 | 538.1 | 9.2x | 2092 | 2508 |
| chain500 | 6.9 | 92.0 | 13.3x | 1968 | 4128 |
| realistic | 968.0 | 1743.9 | 1.8x | 2096 | 2564 |
| rwread4 | 99.1 | 164.8 | 1.7x | 2096 | 2376 |
| rwnested4 | 169.1 | 719.0 | 4.3x | 2096 | 2488 |
| sem4 | 181.0 | 240.3 | 1.3x | 2096 | 2308 |

Start-up cost of one almost-empty process (average of 30 runs): 3.37 ms without LockSight, 10.42 ms with LockSight.

## 2. Detection compared with ThreadSanitizer and Helgrind

| Program | What it is | Truth | LockSight | ThreadSanitizer | Helgrind |
|---|---|---|---|---|---|
| classic_deadlock (--lucky) | two locks, opposite order | BUG | caught | caught | caught |
| three_lock_cycle (--lucky) | cycle over three locks | BUG | caught | caught | caught |
| bank_transfer (--lucky) | transfer(a,b) vs transfer(b,a) | BUG | caught | caught | caught |
| dining_philosophers (--lucky) | 5 philosophers, 5 forks | BUG | caught | caught | caught |
| partial_gate (--lucky) | common lock only for ONE of the two orders | BUG | caught | caught | caught |
| self_deadlock  | same thread locks the same mutex twice | BUG | caught | MISSED | MISSED |
| safe_ordered (--lucky) | all threads lock in the same order | OK | silent (correct) | silent (correct) | silent (correct) |
| gate_lock (--lucky) | opposite orders, but both hold a common outer lock | OK | silent (correct) | FALSE ALARM | FALSE ALARM |
| single_thread_both_orders  | one thread uses both orders, never at once | OK | silent (correct) | FALSE ALARM | FALSE ALARM |
| trylock_case (--lucky) | opposite order, but the 2nd lock uses trylock | OK | silent (correct) | silent (correct) | FALSE ALARM |
| recursive_ok  | recursive mutex locked twice | OK | silent (correct) | silent (correct) | silent (correct) |
| errorcheck_ok  | error-checking mutex, relock handled | OK | silent (correct) | silent (correct) | FALSE ALARM |
| mutex_reuse  | memory of a destroyed mutex reused for a new one | OK | silent (correct) | FALSE ALARM | FALSE ALARM |
| cpp_mutex (--lucky) | C++ std::mutex inversion | BUG | caught | caught | caught |
| rwlock_inversion (--lucky) | two pthread_rwlock_t, opposite order | BUG | caught | caught | caught |
| unexecuted_path  | bad order exists only in a branch not executed | BUG | MISSED | MISSED | MISSED |
| semaphore_deadlock  | sem_wait cycle (no mutex at all) | BUG | MISSED | MISSED | MISSED |
| condvar_lost_wakeup  | signal sent before wait (no lock cycle) | BUG | MISSED | MISSED | MISSED |
| ordered_by_join  | opposite orders, but thread 2 starts only after thread 1 was joined | OK | FALSE ALARM | FALSE ALARM | FALSE ALARM |
| mutex_handoff_ok  | lock taken in one thread, released in another | OK | silent (correct) | silent (correct) | silent (correct) |
| rwlock_mixed (--lucky) | mutex + rwlock (write) in opposite orders | BUG | caught | caught | caught |
| rwlock_rw_cycle (--lucky) | read-lock A then write-lock B, and the reverse | BUG | caught | caught | caught |
| rwlock_upgrade  | read lock upgraded to a write lock by the same thread | BUG | caught | MISSED | MISSED |
| rwlock_readers_ok (--lucky) | only read locks, opposite orders | OK | silent (correct) | FALSE ALARM | FALSE ALARM |
| rwlock_oneway_ok (--lucky) | read A+read B vs write B+read A (no loop) | OK | silent (correct) | FALSE ALARM | FALSE ALARM |
| sem_binary_deadlock (--lucky) | two binary semaphores (value 1) in opposite orders | BUG | caught | MISSED | MISSED |
| sem_binary_ok  | binary semaphores used in one order | OK | silent (correct) | silent (correct) | silent (correct) |
| sem_handoff_ok  | semaphore posted by a thread that never waited (signaling; also one thread uses two mutexes in both orders, which TSan and Helgrind flag on their own) | OK | silent (correct) | FALSE ALARM | FALSE ALARM |

Correct verdicts out of 28 programs: LockSight 24, ThreadSanitizer 15 (n/a if not installed), Helgrind 13.

## 3. Does the tool warn before a real freeze?

| Program (really freezes after 5 s) | LockSight warns before the freeze | ThreadSanitizer warns before the freeze | Helgrind warns before the freeze |
|---|---|---|---|
| classic_deadlock | yes | no warning (froze: 1) | no warning (froze: 1) |
| three_lock_cycle | yes | no warning (froze: 1) | no warning (froze: 1) |
| bank_transfer | yes | no warning (froze: 1) | no warning (froze: 1) |
| dining_philosophers | yes | no warning (froze: 1) | no warning (froze: 1) |
| partial_gate | yes | no warning (froze: 1) | no warning (froze: 1) |
| rwlock_mixed | yes | no warning (froze: 1) | no warning (froze: 1) |
| rwlock_rw_cycle | yes | no warning (froze: 1) | no warning (froze: 1) |
| sem_binary_deadlock | yes | no warning (froze: 1) | no warning (froze: 1) |

## 4. Speed of the three tools

| Scenario | Plain | LockSight | ThreadSanitizer | Helgrind |
|---|---|---|---|---|
| nested4, 200000 iterations | 55 ms (1.0x) | 271 ms (5.0x) | 1415 ms (25.9x) | 11659 ms (213.6x) |
| realistic, 100000 iterations | 99 ms (1.0x) | 173 ms (1.8x) | 10872 ms (110.2x) | 4636 ms (47.0x) |

## 5. Real multithreaded programs

| Program | Exit plain | Exit LockSight | Time plain (s) | Time LockSight (s) | Threads using locks | Locks seen | Edges | Potential deadlocks | Self-deadlocks |
|---|---|---|---|---|---|---|---|---|---|
| xz -T4 (parallel compression) | 0 | 0 | 2.12 | 2.32 | 0 | 0 | 0 | 0 | 0 |
| sort --parallel=4 | 0 | 0 | 0.52 | 0.62 | 0 | 0 | 0 | 0 | 0 |
| git gc --aggressive (threaded pack) | 0 | 0 | 9.14 | 10.44 | 5 | 9 | 0 | 0 | 0 |
| git grep (threaded search) | 0 | 0 | 0.11 | 0.12 | 5 | 5 | 2 | 0 | 0 |
| python3 thread pool (Queue + Lock) | 0 | 0 | 0.23 | 0.22 | 5 | 44 | 66 | 0 | 0 |
| pigz -p4 (parallel gzip) | 0 | 0 | 1.02 | 1.02 | 6 | 407 | 0 | 0 | 0 |
| zstd -T4 (parallel compression) | 0 | 0 | 0.32 | 0.43 | 7 | 15 | 0 | 0 | 0 |
| ffmpeg, 4 encoder threads | 0 | 0 | 0.42 | 0.42 | 19 | 47 | 19 | 0 | 0 |
| redis-server with 4 I/O threads | 0 | 0 | 26.24 | 27.13 | 9 | 181 | 56 | 0 | 0 |

Times are the median of 3 runs. Exit 3 under LockSight means it reported a problem; look at the matching file in bench/results/logs/ and decide whether it is real or a false alarm.
