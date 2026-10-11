#!/usr/bin/env bash
# real_software.sh - runs LockSight on real multithreaded programs that were
# NOT written for this project. Programs that are not installed are skipped.
# Optional extras:  sudo apt install pigz zstd memcached ffmpeg redis-server git xz-utils
#
# For every program: does it still work under LockSight (same exit status)?
# how much slower is it? did LockSight warn (and is the warning real)?
# Output: bench/results/real_software.md   (full reports: bench/results/logs/)
cd "$(dirname "$0")" || exit 1
ROOT=$PWD/..
LS="$ROOT/locksight"
[ -f "$ROOT/src/liblocksight.so" ] || { echo "run 'make' in the LockSight folder first"; exit 1; }
mkdir -p results/logs build/real
W=$PWD/build/real
OUT=results/real_software.md

# ---- test data (made once) -----------------------------------------------------
if [ ! -f "$W/data.txt" ]; then
    echo "making test data..."
    # ~40 MB of semi-compressible text
    awk 'BEGIN{srand(7); for(i=0;i<900000;i++){ printf "line %d value %d key%d the quick brown fox %d\n", i, int(rand()*1e6), int(rand()*5000), int(rand()*100)}}' > "$W/data.txt"
fi
if [ ! -d "$W/repo/.git" ]; then
    mkdir -p "$W/repo" && ( cd "$W/repo" && git init -q . \
      && for i in $(seq 1 60); do head -c 300000 ../data.txt | sed "s/line/f$i/" > "file$i.txt"; done \
      && git add . && git -c user.name=t -c user.email=t@t commit -q -m init ) >/dev/null 2>&1
fi
cat > "$W/pool.py" <<'PY'
import threading, queue, hashlib
q = queue.Queue(); res = []; lock = threading.Lock()
def worker():
    while True:
        x = q.get()
        if x is None: return
        h = hashlib.sha256(str(x).encode()).hexdigest()
        with lock: res.append(h)
ts = [threading.Thread(target=worker) for _ in range(4)]
[t.start() for t in ts]
for i in range(20000): q.put(i)
for _ in ts: q.put(None)
[t.join() for t in ts]
print(len(res))
PY
cat > "$W/ffmpeg_job.sh" <<'SH'
#!/usr/bin/env bash
# a small threaded video encode: 3 s synthetic test video -> mpeg4, 4 threads
ffmpeg -nostdin -v error -y -f lavfi -i testsrc=duration=3:size=640x360:rate=25 -c:v mpeg4 -threads 4 "$1/out.mp4"
SH
chmod +x "$W/ffmpeg_job.sh"
cat > "$W/redis_job.sh" <<'SH'
#!/usr/bin/env bash
# start redis-server with I/O threads, send some commands, shut it down cleanly
PORT=16399
redis-server --port $PORT --io-threads 4 --save "" --appendonly no --daemonize no >/dev/null 2>&1 &
RP=$!
for i in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.1; done
for i in $(seq 200); do
  exec 3<>/dev/tcp/127.0.0.1/$PORT
  printf 'SET k%d v%d\r\nGET k%d\r\nQUIT\r\n' $i $i $i >&3
  timeout 5 cat <&3 >/dev/null; exec 3>&-
done
exec 3<>/dev/tcp/127.0.0.1/$PORT; printf 'SHUTDOWN NOSAVE\r\n' >&3; exec 3>&-
for i in $(seq 100); do kill -0 $RP 2>/dev/null || break; sleep 0.1; done
kill -9 $RP 2>/dev/null; wait $RP 2>/dev/null
SH
chmod +x "$W/redis_job.sh"

# name | command (use ${W} for the work dir)
CASES=(
"xz -T4 (parallel compression)|xz -T4 -1 -c $W/data.txt"
"sort --parallel=4|sort --parallel=4 -S 200M $W/data.txt"
"git gc --aggressive (threaded pack)|git -C $W/repo -c pack.threads=4 gc --aggressive -q"
"git grep (threaded search)|git -C $W/repo -c grep.threads=4 grep -c quick"
"python3 thread pool (Queue + Lock)|python3 $W/pool.py"
"pigz -p4 (parallel gzip)|pigz -p4 -c $W/data.txt"
"zstd -T4 (parallel compression)|zstd -T4 -q -c $W/data.txt"
"ffmpeg, 4 encoder threads|$W/ffmpeg_job.sh $W"
"redis-server with 4 I/O threads|$W/redis_job.sh"
)

secs() { date +%s.%N; }
{
echo "| Program | Exit plain | Exit LockSight | Time plain (s) | Time LockSight (s) | Threads using locks | Locks seen | Edges | Potential deadlocks | Self-deadlocks |"
echo "|---|---|---|---|---|---|---|---|---|---|"
} > $OUT
FLAGGED=""
for c in "${CASES[@]}"; do
    name=${c%%|*}; cmd=${c#*|}
    prog=$(echo "$cmd" | awk '{print $1}')
    case $prog in $W/*) ;; *) command -v "$prog" >/dev/null || { echo "  skip: $name ($prog not installed)"; echo "| $name | not installed | | | | | | | | |" >> $OUT; continue; };; esac
    if [ "$prog" = "$W/ffmpeg_job.sh" ] && ! command -v ffmpeg >/dev/null; then echo "  skip: $name"; echo "| $name | not installed | | | | | | | | |" >> $OUT; continue; fi
    if [ "$prog" = "$W/redis_job.sh" ] && ! command -v redis-server >/dev/null; then echo "  skip: $name"; echo "| $name | not installed | | | | | | | | |" >> $OUT; continue; fi
    safe=$(echo "$name" | tr -c 'A-Za-z0-9' '_')
    log=results/logs/$safe.txt
    echo "  running: $name"
    # 3 runs each, report the median time
    pts=(); lts=()
    for r in 1 2 3; do
        t0=$(secs); timeout -k 5 60 bash -c "$cmd" </dev/null >/dev/null 2>&1; e0=$?; t1=$(secs)
        LOCKSIGHT_COLOR=never timeout -k 5 60 "$LS" run --log "$PWD/$log" bash -c "$cmd" </dev/null >/dev/null 2>&1; e1=$?; t2=$(secs)
        pts+=("$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.2f", b-a}')")
        lts+=("$(awk -v a=$t1 -v b=$t2 'BEGIN{printf "%.2f", b-a}')")
    done
    pt=$(printf '%s\n' "${pts[@]}" | sort -n | sed -n 2p)
    lt=$(printf '%s\n' "${lts[@]}" | sort -n | sed -n 2p)
    # one process may print several summaries (a script that starts programs): sum them
    sum() { grep -E "^ +$1 " "$log" 2>/dev/null | awk '{ for(i=NF;i>0;i--) if ($i ~ /^[0-9]+$/) { s+=$i; break } } END{print s+0}'; }
    th=$(sum "threads that used locks")
    lk=$(grep -E "^ +locks seen" "$log" 2>/dev/null | awk '{s+=$3} END{print s+0}')
    ed=$(sum "lock-order edges")
    pd=$(sum "potential deadlocks")
    sd=$(sum "self-deadlocks")
    echo "| $name | $e0 | $e1 | $pt | $lt | $th | $lk | $ed | $pd | $sd |" >> $OUT
    [ "$e1" = 3 ] && FLAGGED="$FLAGGED $name;"
    echo "  $name: plain exit $e0 / LockSight exit $e1, ${pt}s -> ${lt}s, warnings $pd+$sd"
done
echo >> $OUT
echo "Times are the median of 3 runs. Exit 3 under LockSight means it reported a problem; look at the matching file in bench/results/logs/ and decide whether it is real or a false alarm." >> $OUT
[ -n "$FLAGGED" ] && echo "Programs LockSight flagged:$FLAGGED" >> $OUT
echo; cat $OUT
