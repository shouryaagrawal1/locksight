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
