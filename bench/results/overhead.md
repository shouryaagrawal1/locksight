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
