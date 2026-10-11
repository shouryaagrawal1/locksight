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
