| Scenario | Plain | LockSight | ThreadSanitizer | Helgrind |
|---|---|---|---|---|
| nested4, 200000 iterations | 55 ms (1.0x) | 271 ms (5.0x) | 1415 ms (25.9x) | 11659 ms (213.6x) |
| realistic, 100000 iterations | 99 ms (1.0x) | 173 ms (1.8x) | 10872 ms (110.2x) | 4636 ms (47.0x) |
