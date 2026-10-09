# LockSight: Dynamic Deadlock Predictor for C/C++

LockSight is an ultra-lightweight dynamic analysis tool that finds hidden deadlock vulnerabilities in multithreaded C/C++ applications. By intercepting POSIX thread synchronization calls via `LD_PRELOAD`, LockSight maps out your program's internal lock-order graph at runtime and mathematically guarantees the presence of structural deadlocks—even if the OS scheduler happens to give you a "lucky" execution where the threads never actually collide.

Unlike heavyweight data-race detectors (like ThreadSanitizer or Helgrind) that intercept every memory access and impose massive slowdowns, LockSight strictly tracks synchronization logic using a zero-allocation, static-pool architecture. This allows it to run on pre-compiled, unmodified binaries with sub-microsecond overhead, making it perfectly suited for high-load staging environments and automated CI/CD pipelines.

## Key Features

* **Zero-Modification Execution:** Runs on any dynamically linked C/C++ binary via `LD_PRELOAD`. No recompilation or compiler flags required.
* **Preemptive Prediction:** Detects lock inversions (e.g., AB-BA deadlocks) deterministically on the very first test run, catching elusive 1% "Heisenbugs" without needing the program to actually freeze.
* **High-Precision Filtering:** Implements the GoodLock algorithm with gate-lock filtering and thread-ownership validation to eliminate false positives.
* **Self-Deadlock Detection:** Immediately flags threads attempting to double-lock non-recursive mutexes.
* **Zero Dynamic Allocation:** Avoids `malloc` and `free` entirely during tracking, utilizing fixed-size static pools to prevent allocator lock contention and maintain blistering execution speeds. 
* **CI/CD Ready:** Features a `--strict` fail-fast mode and automated exit codes (returns `3` on detected cycles) for seamless integration into testing pipelines.
