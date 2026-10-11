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
