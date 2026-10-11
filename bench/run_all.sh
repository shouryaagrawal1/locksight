#!/usr/bin/env bash
# run_all.sh - Phase 5: runs every measurement and writes bench/results/summary.md
# (takes a few minutes; do not use the computer for other heavy work meanwhile,
#  otherwise the timing numbers are noisy).
cd "$(dirname "$0")" || exit 1
mkdir -p results
echo "== 1/3 overhead";        ./overhead.sh       > results/overhead.log 2>&1 || { echo "overhead.sh failed, see results/overhead.log"; exit 1; }
echo "== 2/3 tool comparison"; ./compare_tools.sh  > results/compare.log  2>&1 || { echo "compare_tools.sh failed, see results/compare.log"; exit 1; }
echo "== 3/3 real software";   ./real_software.sh  > results/real.log     2>&1 || { echo "real_software.sh failed, see results/real.log"; exit 1; }

CPU=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')
{
echo "# LockSight measurements"
echo
echo "Machine: $CPU, $(nproc) logical CPUs, $(uname -sr), $(gcc --version | head -1)"
command -v valgrind >/dev/null && echo "Valgrind: $(valgrind --version)"
echo "Date: $(date +%F)"
echo
echo "## 1. Overhead (median of 5 runs)"; echo; cat results/overhead.md
echo; echo "## 2. Detection compared with ThreadSanitizer and Helgrind"; echo; cat results/detection.md
echo; echo "## 3. Does the tool warn before a real freeze?"; echo; cat results/before_freeze.md
echo; echo "## 4. Speed of the three tools"; echo; cat results/speed.md
echo; echo "## 5. Real multithreaded programs"; echo; cat results/real_software.md
} > results/summary.md
echo
echo "Done. Everything is in bench/results/summary.md"
