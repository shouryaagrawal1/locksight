/*
 * liblocksight.c - LockSight: finds potential deadlocks before they happen.
 *
 * HOW IT WORKS (in one paragraph)
 * -------------------------------
 * The program is started with LD_PRELOAD=./liblocksight.so. Linux's dynamic
 * linker then loads THIS library first, so when the program calls
 * pthread_mutex_lock(), it lands in OUR pthread_mutex_lock() below instead of
 * the real one. Our version (1) notes which locks the calling thread already
 * holds, (2) records an edge "held lock -> requested lock" in a global graph,
 * (3) checks whether that edge closes a loop (a cycle = a possible deadlock),
 * and only then (4) calls the REAL pthread_mutex_lock, found with
 * dlsym(RTLD_NEXT, ...). The target program is never modified.
 *
 * WHY A CYCLE MEANS "POSSIBLE DEADLOCK"
 * -------------------------------------
 * Edge A -> B means "some thread asked for B while holding A".
 * If the graph also has B -> A, then two threads can each hold one lock and
 * wait for the other. Longer loops (A -> B -> C -> A) work the same way.
 *
 * WHEN A CYCLE IS *NOT* A REAL DANGER (Phase 3 filters)
 * -----------------------------------------------------
 * A loop in the graph is only reported if it could really happen:
 *   1. The edges of the loop must come from DIFFERENT threads. One thread
 *      using both orders (A then B, later B then A) can never wait for itself.
 *   2. The threads must share NO "gate" lock. If every thread holds the same
 *      outer lock G while taking A and B, only one of them can be inside at a
 *      time, so they can never block each other.
 * To check (2), every edge remembers which locks its thread held at that
 * moment. (This is the idea of the "GoodLock" family of detectors.)
 *
 * READ-WRITE LOCKS (Phase 6)
 * ---------------------------
 * pthread_rwlock_rdlock / wrlock (and try / timed variants) are tracked with
 * a MODE: a read lock is SHARED, a write lock is EXCLUSIVE. Readers do not
 * block each other, so the two filters above become mode-aware:
 *   - A step "thread X holds L and asks for M" only blocks on a thread that
 *     holds M in a CONFLICTING mode (anything except read with read).
 *   - A lock held by two threads is a "gate" only if at least one of them
 *     holds it exclusively. Two readers of the same lock are not kept apart.
 * Every edge therefore stores the mode it was held in and the mode requested,
 * plus a second bit set with the locks held exclusively.
 * (glibc's default lets readers go first, so a reader is not blocked by a
 * WAITING writer; a program that asks for writer preference could deadlock
 * in ways this model does not see.)
 *
 * BINARY SEMAPHORES (Phase 6)
 * ---------------------------
 * A semaphore has no owner. sem_init(&s, 0, 1) (value 1) makes LockSight treat
 * it as a lock: the thread that sem_wait()s it holds it until its own
 * sem_post(). Semaphores with another initial value are signals and are not
 * tracked. If a tracked semaphore is posted by a thread that never waited on
 * it, it is really a signal: LockSight prints a note and removes it from the
 * graph. Other threads' lists of held locks are never edited from outside:
 * they carry a generation number, and an entry from an older generation is
 * ignored (the same mechanism handles a mutex released by another thread).
 *
 * OTHER THINGS HANDLED
 * --------------------
 *  - Self-deadlock: a thread locks a normal mutex it already holds, or asks
 *    for a write lock while holding a read lock on the same rwlock.
 *  - Recursive / error-checking mutexes may be locked again: no warning.
 *  - pthread_mutex_destroy / pthread_mutex_init: the history of a mutex is
 *    forgotten, because the same memory address may later hold a new mutex.
 *  - trylock and timed locks never wait forever, so they add no edge.
 *  - LockSight's own work (printing, addr2line) is never tracked
 *    (the "re-entrancy guard" t_busy).
 *
 * OPTIONS
 * -------
 * Normally you do not use this file directly but through the "locksight"
 * launcher script, which turns its command-line options into environment
 * variables read by this library (see Part 2): --quiet, --log, --strict.
 * Colors are used only when the output is a terminal.
 *
 * Build:  gcc -shared -fPIC -O2 -g -pthread -o liblocksight.so liblocksight.c -ldl
 * Use:    LD_PRELOAD=./src/liblocksight.so ./tests/classic_deadlock --lucky
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Limits. Fixed-size tables mean we never call malloc while tracking, */
/* which keeps the interceptor simple and safe.                        */
/* ------------------------------------------------------------------ */
/* The size limits can be changed when building, for example
 *     make LIMITS="-DMAX_LOCKS=1000000 -DMAX_EDGES=1000000"
 * (HASH_SIZE and PAIR_HASH follow automatically). The memory is reserved but
 * only used as locks and edges really appear. */
#ifndef MAX_LOCKS
#define MAX_LOCKS      65536    /* locks tracked at the same time            */
#endif
#ifndef HASH_SIZE
#define HASH_SIZE      (4 * MAX_LOCKS)  /* lock-address hash table           */
#endif
#ifndef MAX_EDGES
#define MAX_EDGES      262144   /* "A held while taking B" records           */
#endif
#ifndef PAIR_HASH
#define PAIR_HASH      MAX_EDGES        /* index of edges by (from, to)      */
#endif
#define MAX_HELD       64       /* locks one thread can hold at once         */
#define MAX_FRAMES     8        /* call-stack frames saved per edge          */
#define MAX_CYCLE      16       /* longest cycle printed in full             */
#define MAX_DEPTH      256      /* longest cycle we search for               */
#define MAX_PER_EDGE   4        /* different threads remembered per A->B pair
                                   (and per combination of lock modes)        */
#define GATE_BITS      256      /* locks 0..255 can act as "gate" locks      */
#define GATE_WORDS     (GATE_BITS / 64)
#define SEARCH_BUDGET  200000   /* edges one cycle search may look at        */
#define SEARCH_TOTAL   60000000 /* edges ALL cycle searches may look at; after
                                   that new edges are no longer checked       */
#define MAX_BACK       4096     /* "cycle" edges set aside by the lock ordering */
#define MAX_WARNINGS   20       /* stop printing after this many warnings    */
#define OUT_SIZE       24576    /* size of the warning text buffer           */

/* What kind of synchronization object a "lock" is, and in which MODE a thread
 * holds it or asks for it. A read lock (pthread_rwlock_rdlock) is SHARED: any
 * number of readers can hold it together. Everything else is EXCLUSIVE: mutex,
 * write lock, binary semaphore. Two modes CONFLICT unless both are shared. */
#define KIND_MUTEX   0
#define KIND_RWLOCK  1
#define KIND_SEM     2
#define MODE_X       0
#define MODE_S       1

static int conflicts(int mode_a, int mode_b)
{
    return !(mode_a == MODE_S && mode_b == MODE_S);
}

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

/* One tracked lock (mutex, rwlock or binary semaphore). We identify a lock by
 * its memory address. */
typedef struct {
    void    *addr;     /* address of the pthread_mutex_t / rwlock_t / sem_t */
    void    *first_pc; /* where in the program we first saw it being used   */
    int      kind;     /* KIND_MUTEX, KIND_RWLOCK or KIND_SEM               */
    unsigned gen;      /* generation: raised whenever "who holds this lock"
                          becomes unknown (flush / hand-off). A thread's own
                          list of held locks remembers the generation it saw;
                          entries with an old generation are ignored.        */
} LockInfo;

/* One record of the lock-order graph:  "thread 'tid' asked for lock 'to'
 * while holding lock 'from'".  The same pair from -> to can have a few
 * records, one per thread (up to MAX_PER_EDGE). */
typedef struct {
    int      from, to;               /* lock ids                              */
    int      tid;                    /* the thread this record belongs to     */
    unsigned char hmode, rmode;      /* mode in which 'from' was HELD / 'to'
                                        was REQUESTED (MODE_X or MODE_S)      */
    unsigned char hkind, rkind;      /* KIND_* of 'from' and of 'to'          */
    int      next_out, next_in;      /* lists of edges leaving 'from' and
                                        entering 'to' (index+1, 0 = end)      */
    int      prev_out, prev_in;      /* the same lists backwards, so that an
                                        edge can be removed in O(1)           */
    int      next_pair;              /* chain of edges with the same (from,to)
                                        hash bucket (index+1, 0 = end)        */
    unsigned char back;              /* 1 = this edge closes a loop in the plain
                                        lock graph; it is set aside from the
                                        lock ordering (see "Lock ordering")   */
    uint64_t held[GATE_WORDS];       /* set of locks the thread held at that
                                        moment (bit n = lock id n)            */
    uint64_t hx[GATE_WORDS];         /* the subset held EXCLUSIVELY (only these
                                        can keep another thread out)          */
    void    *from_pc;                /* where 'from' had been acquired        */
    void    *to_stack[MAX_FRAMES];   /* call stack where 'to' was requested   */
    int      to_depth;               /* number of valid entries in to_stack   */
} Edge;

/* One lock currently held by the calling thread. */
typedef struct {
    void    *addr;
    int      id;
    int      mode;     /* MODE_X or MODE_S                    */
    unsigned gen;      /* generation of the lock when taken   */
    void    *pc;       /* where it was acquired               */
} Held;

/* A cycle copied out of the graph so we can print it without holding locks. */
typedef struct {
    Edge     e[MAX_CYCLE];
    LockInfo l[MAX_CYCLE];          /* l[i] describes lock e[i].from          */
    int      n;                     /* edges stored                           */
    int      total;                 /* real length of the cycle               */
    int      number;                /* warning number                         */
} Cycle;

/* A thread that asks for a normal mutex it already holds. */
typedef struct {
    LockInfo lock;
    void    *held_pc;               /* where the thread took the lock first   */
    void    *req_stack[MAX_FRAMES]; /* where it asks for it again             */
    int      req_depth;
    int      id, tid, number;
    int      kind;                  /* KIND_MUTEX or KIND_RWLOCK              */
} SelfDeadlock;

/* ------------------------------------------------------------------ */
/* Global state (protected by g_mutex, a lock WE take with the REAL     */
/* functions so it is never tracked).                                   */
/* ------------------------------------------------------------------ */
static LockInfo g_locks[MAX_LOCKS];
static int      g_lock_free[MAX_LOCKS];     /* ids of forgotten locks, for reuse */
static int      g_nlock_free;
static int      g_nlocks_hw;                /* ids 0..hw-1 have been handed out  */
static int      g_nlocks_live;              /* locks currently tracked           */
static int      g_nlocks_seen;              /* locks ever seen (for the summary) */
static int      g_slot[HASH_SIZE];          /* address hash: 0 = empty, else id+1 */

static Edge     g_edges[MAX_EDGES];
static int      g_edge_free[MAX_EDGES];     /* indices of freed edges, for reuse */
static int      g_nedge_free;
static int      g_nedges_hw;
static int      g_nedges_seen;
static int      g_first_out[MAX_LOCKS];     /* per lock: first edge leaving it + 1 */
static int      g_first_in[MAX_LOCKS];      /* per lock: first edge entering it + 1 */
static int      g_pair_head[PAIR_HASH];     /* (from,to) hash bucket: first edge + 1 */
static uint64_t g_ord[MAX_LOCKS];           /* position of each lock in the lock ordering */
static uint64_t g_next_ord;
static int      g_back[MAX_BACK];           /* edges set aside (flag "back")        */
static int      g_nback, g_back_overflow;
static long     g_search_total;             /* edges looked at by all searches so far */

/* Scratch space for the cycle search. */
static unsigned g_mark[MAX_LOCKS];          /* "on the current path" marks     */
static unsigned g_gen;
static int      g_chosen[MAX_DEPTH];        /* edge indices of the current path */
static int      g_depth;
static long     g_budget;

static int      g_nthreads, g_ncycles, g_nself;
static int      g_dropped_locks, g_dropped_edges, g_budget_hits;
static int      g_warnings_printed;
static int      g_nnotes;                   /* semaphores dropped from tracking  */

static pthread_mutex_t g_mutex       = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_print_mutex = PTHREAD_MUTEX_INITIALIZER;
static char            g_out[OUT_SIZE];

/* ------------------------------------------------------------------ */
/* Per-thread state                                                    */
/* ------------------------------------------------------------------ */
static __thread Held t_held[MAX_HELD];      /* locks this thread holds, in order */
static __thread int  t_nheld;
static __thread int  t_tid;                 /* small thread number (T1, T2...)   */
static __thread int  t_busy;                /* re-entrancy guard (see below)     */

/* ------------------------------------------------------------------ */
/* The REAL pthread functions, looked up with dlsym(RTLD_NEXT, ...)    */
/* ------------------------------------------------------------------ */
static int (*real_lock)(pthread_mutex_t *);
static int (*real_trylock)(pthread_mutex_t *);
static int (*real_unlock)(pthread_mutex_t *);
static int (*real_init)(pthread_mutex_t *, const pthread_mutexattr_t *);
static int (*real_destroy)(pthread_mutex_t *);
static int (*real_timedlock)(pthread_mutex_t *, const struct timespec *);

static int (*real_rdlock)(pthread_rwlock_t *);
static int (*real_wrlock)(pthread_rwlock_t *);
static int (*real_tryrdlock)(pthread_rwlock_t *);
static int (*real_trywrlock)(pthread_rwlock_t *);
static int (*real_timedrdlock)(pthread_rwlock_t *, const struct timespec *);
static int (*real_timedwrlock)(pthread_rwlock_t *, const struct timespec *);
static int (*real_rw_unlock)(pthread_rwlock_t *);
static int (*real_rw_init)(pthread_rwlock_t *, const pthread_rwlockattr_t *);
static int (*real_rw_destroy)(pthread_rwlock_t *);

static int (*real_sem_init)(sem_t *, int, unsigned int);
static int (*real_sem_destroy)(sem_t *);
static int (*real_sem_wait)(sem_t *);
static int (*real_sem_trywait)(sem_t *);
static int (*real_sem_timedwait)(sem_t *, const struct timespec *);
static int (*real_sem_post)(sem_t *);
#if defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 30)
static int (*real_sem_clockwait)(sem_t *, clockid_t, const struct timespec *);
#endif
#endif

static void init_real(void)
{
    if (real_lock && real_trylock && real_unlock && real_init && real_destroy &&
        real_sem_post && real_rw_unlock)
        return;
    /* Looking these up twice (two threads racing) is harmless. */
    real_lock    = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_trylock = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_unlock  = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    real_init    = (int (*)(pthread_mutex_t *, const pthread_mutexattr_t *))
                       dlsym(RTLD_NEXT, "pthread_mutex_init");
    real_destroy = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_destroy");
    real_timedlock = (int (*)(pthread_mutex_t *, const struct timespec *))
                         dlsym(RTLD_NEXT, "pthread_mutex_timedlock");

    real_rdlock  = (int (*)(pthread_rwlock_t *))dlsym(RTLD_NEXT, "pthread_rwlock_rdlock");
    real_wrlock  = (int (*)(pthread_rwlock_t *))dlsym(RTLD_NEXT, "pthread_rwlock_wrlock");
    real_tryrdlock = (int (*)(pthread_rwlock_t *))dlsym(RTLD_NEXT, "pthread_rwlock_tryrdlock");
    real_trywrlock = (int (*)(pthread_rwlock_t *))dlsym(RTLD_NEXT, "pthread_rwlock_trywrlock");
    real_timedrdlock = (int (*)(pthread_rwlock_t *, const struct timespec *))
                           dlsym(RTLD_NEXT, "pthread_rwlock_timedrdlock");
    real_timedwrlock = (int (*)(pthread_rwlock_t *, const struct timespec *))
                           dlsym(RTLD_NEXT, "pthread_rwlock_timedwrlock");
    real_rw_init = (int (*)(pthread_rwlock_t *, const pthread_rwlockattr_t *))
                       dlsym(RTLD_NEXT, "pthread_rwlock_init");
    real_rw_destroy = (int (*)(pthread_rwlock_t *))dlsym(RTLD_NEXT, "pthread_rwlock_destroy");
    real_rw_unlock = (int (*)(pthread_rwlock_t *))dlsym(RTLD_NEXT, "pthread_rwlock_unlock");

    real_sem_init    = (int (*)(sem_t *, int, unsigned int))dlsym(RTLD_NEXT, "sem_init");
    real_sem_destroy = (int (*)(sem_t *))dlsym(RTLD_NEXT, "sem_destroy");
    real_sem_wait    = (int (*)(sem_t *))dlsym(RTLD_NEXT, "sem_wait");
    real_sem_trywait = (int (*)(sem_t *))dlsym(RTLD_NEXT, "sem_trywait");
    real_sem_timedwait = (int (*)(sem_t *, const struct timespec *))
                             dlsym(RTLD_NEXT, "sem_timedwait");
    real_sem_post    = (int (*)(sem_t *))dlsym(RTLD_NEXT, "sem_post");
#if defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 30)
    real_sem_clockwait = (int (*)(sem_t *, clockid_t, const struct timespec *))
                             dlsym(RTLD_NEXT, "sem_clockwait");
#endif
#endif
}

/* ================================================================== */
/* Part 1: turning addresses into "file.c:42 (function)"               */
/* ================================================================== */
/*
 * Starting another program (addr2line) is SLOW compared with everything else
 * LockSight does, so we never start one per address. print_cycle() first
 * collects every address it will print and hands them all to resolve_batch(),
 * which starts ONE addr2line per file (normally just the program itself).
 * Addresses inside system libraries (libc, ...) are not looked up at all:
 * they are never interesting to the person reading the warning.
 * Answers are cached, so each address is looked up at most once.
 * (All of this is only used while holding g_print_mutex.)
 */
#define CACHE_SIZE   1024
#define TODO_MAX     256
#define TEXT_LEN     256
#define A2L_CHUNK    48         /* addresses given to one addr2line run   */

typedef struct {
    void *pc;
    int   has_line;             /* 1 = text is "file.c:42 (function)"      */
    int   is_system;            /* 1 = inside libc / loader / libstdc++    */
    char  text[TEXT_LEN];
} Resolved;

typedef struct {
    Resolved *r;                /* cache entry to fill in                  */
    uintptr_t off;              /* address as it was when the file was linked */
    char      file[400];        /* file to give to addr2line               */
    int       done;
} Todo;

static Resolved g_cache[CACHE_SIZE];
static int      g_ncache;
static Todo     g_todo[TODO_MAX];

typedef struct {
    uintptr_t addr;        /* address we are looking for              */
    char      name[512];   /* file the address belongs to             */
    uintptr_t bias;        /* load offset of that file (ASLR)         */
    int       found;
} FindCtx;

/* Called once per loaded file (the program, libc, ...). We look for the one
 * whose memory range contains the address. */
static int find_object_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    FindCtx *c = (FindCtx *)data;
    (void)size;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD)
            continue;
        uintptr_t lo = info->dlpi_addr + ph->p_vaddr;
        uintptr_t hi = lo + ph->p_memsz;
        if (c->addr >= lo && c->addr < hi) {
            const char *nm = info->dlpi_name ? info->dlpi_name : "";
            snprintf(c->name, sizeof c->name, "%s", nm);
            c->bias  = info->dlpi_addr;
            c->found = 1;
            return 1;               /* stop iterating */
        }
    }
    return 0;
}

static int is_system_file(const char *p)
{
    static const char *const names[] = {
        "/libc.so", "/ld-linux", "/libpthread", "/libdl", "/libm.so",
        "/libgcc_s", "/libstdc++", "linux-vdso"
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strstr(p, names[i]))
            return 1;
    return 0;
}

static Resolved *cache_find(void *pc)
{
    for (int i = 0; i < g_ncache; i++)
        if (g_cache[i].pc == pc)
            return &g_cache[i];
    return NULL;
}

/* addr2line -p prints one line per address:   function at /path/file.c:42   */
static void parse_a2l_line(char *line, Resolved *r)
{
    line[strcspn(line, "\n")] = '\0';
    char *at = strstr(line, " at ");
    if (!at)
        return;
    *at = '\0';
    const char *func = line;
    char *loc = at + 4;

    char *paren = strstr(loc, " (");             /* drop "(discriminator N)" */
    if (paren)
        *paren = '\0';
    if (loc[0] == '?') {                         /* no line info available   */
        snprintf(r->text, TEXT_LEN, "%.200s (no line info; compile with -g)", func);
        return;
    }
    char *slash = strrchr(loc, '/');
    if (slash)
        loc = slash + 1;                         /* keep only the file name  */
    snprintf(r->text, TEXT_LEN, "%.120s (%.120s)", loc, func);
    r->has_line = 1;
}

/* Look up all the addresses in pcs[0..n-1] that are not in the cache yet. */
static void resolve_batch(void **pcs, int n)
{
    int nt = 0;

    /* Step 1: for each new address, find out which file it belongs to. */
    for (int i = 0; i < n && nt < TODO_MAX; i++) {
        void *pc = pcs[i];
        if (!pc || cache_find(pc) || g_ncache >= CACHE_SIZE)
            continue;

        Resolved *r = &g_cache[g_ncache++];
        memset(r, 0, sizeof *r);
        r->pc = pc;
        snprintf(r->text, TEXT_LEN, "unknown (%p)", pc);

        /* A return address points just AFTER the call, so look one byte back. */
        FindCtx c;
        memset(&c, 0, sizeof c);
        c.addr = (uintptr_t)pc - 1;
        dl_iterate_phdr(find_object_cb, &c);
        if (!c.found)
            continue;

        char exe[512];
        const char *file = c.name;
        if (file[0] == '\0') {                   /* empty name = main program */
            ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
            if (len <= 0)
                continue;
            exe[len] = '\0';
            file = exe;
        }
        if (is_system_file(file)) {              /* not worth an addr2line run */
            const char *base = strrchr(file, '/');
            r->is_system = 1;
            snprintf(r->text, TEXT_LEN, "%.200s", base ? base + 1 : file);
            continue;
        }
        if (strchr(file, '\'') != NULL || strlen(file) >= sizeof g_todo[0].file)
            continue;                            /* keep the shell command safe */

        g_todo[nt].r    = r;
        g_todo[nt].off  = c.addr - c.bias;       /* addr2line wants the address
                                                    as it was when linked (ASLR) */
        g_todo[nt].done = 0;
        snprintf(g_todo[nt].file, sizeof g_todo[nt].file, "%s", file);
        nt++;
    }

    /* Step 2: one addr2line run per file, giving it all of that file's
     * addresses at once. 'env -u LD_PRELOAD' stops addr2line itself from
     * loading LockSight. */
    for (int i = 0; i < nt; i++) {
        if (g_todo[i].done)
            continue;

        int  idx[A2L_CHUNK], k = 0;
        char cmd[A2L_CHUNK * 24 + 600];
        int  len = snprintf(cmd, sizeof cmd,
                            "env -u LD_PRELOAD addr2line -f -C -p -e '%s'",
                            g_todo[i].file);
        for (int j = i; j < nt && k < A2L_CHUNK; j++) {
            if (g_todo[j].done || strcmp(g_todo[j].file, g_todo[i].file) != 0)
                continue;
            len += snprintf(cmd + len, sizeof cmd - (size_t)len, " 0x%lx",
                            (unsigned long)g_todo[j].off);
            g_todo[j].done = 1;
            idx[k++] = j;
        }
        snprintf(cmd + len, sizeof cmd - (size_t)len, " 2>/dev/null");

        FILE *fp = popen(cmd, "r");
        if (!fp)
            continue;
        char line[1024];
        for (int m = 0; m < k && fgets(line, sizeof line, fp); m++) {
            size_t L = strlen(line);
            if (L > 0 && line[L - 1] != '\n') {  /* very long line: skip the rest */
                int ch;
                while ((ch = fgetc(fp)) != EOF && ch != '\n') { }
            }
            parse_a2l_line(line, g_todo[idx[m]].r);
        }
        pclose(fp);
    }
}

/* Fills 'out' with the text for one address. If the address was not looked up
 * in advance, it is looked up now. *has_line is set to 1 when a real source
 * line was found (and the address is not inside a system library). */
static void describe(void *pc, char *out, size_t cap, int *has_line)
{
    if (has_line)
        *has_line = 0;
    if (!pc) {
        snprintf(out, cap, "unknown");
        return;
    }
    Resolved *r = cache_find(pc);
    if (!r) {
        resolve_batch(&pc, 1);
        r = cache_find(pc);
    }
    if (!r) {
        snprintf(out, cap, "unknown (%p)", pc);
        return;
    }
    snprintf(out, cap, "%s", r->text);
    if (has_line)
        *has_line = r->has_line;
}

/* ================================================================== */
/* Part 2: printing (colors, log file, status file, summary)           */
/* ================================================================== */

/*
 * Settings come from environment variables, which the "locksight" launcher
 * sets for you from its command-line options:
 *
 *   LOCKSIGHT_QUIET=1     only warnings, no summary           (--quiet)
 *   LOCKSIGHT_STRICT=1    stop the program at the first problem (--strict)
 *   LOCKSIGHT_LOG=file    also append a plain-text copy to a file (--log)
 *   LOCKSIGHT_JSON=file   also append one JSON object per line (JSON Lines)
 *                         for every event, for scripts and dashboards (--json)
 *   LOCKSIGHT_STATUS=file the launcher reads this to learn whether anything
 *                         was reported, so it can return exit status 3
 *   LOCKSIGHT_COLOR=always|never|auto   (auto = only when stderr is a terminal)
 *   NO_COLOR=1            turns colors off (a common convention)
 */
static int  g_config_loaded;
static int  g_quiet, g_strict, g_color;
static char g_logpath[1024];
static char g_statuspath[1024];
static char g_jsonpath[1024];

static int env_flag(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

static void load_config(void)
{
    if (g_config_loaded)
        return;
    g_config_loaded = 1;

    g_quiet  = env_flag("LOCKSIGHT_QUIET");
    g_strict = env_flag("LOCKSIGHT_STRICT");

    const char *v = getenv("LOCKSIGHT_LOG");
    if (v)
        snprintf(g_logpath, sizeof g_logpath, "%s", v);
    v = getenv("LOCKSIGHT_JSON");
    if (v)
        snprintf(g_jsonpath, sizeof g_jsonpath, "%s", v);
    v = getenv("LOCKSIGHT_STATUS");
    if (v)
        snprintf(g_statuspath, sizeof g_statuspath, "%s", v);

    const char *mode = getenv("LOCKSIGHT_COLOR");
    if (mode && strcmp(mode, "always") == 0)
        g_color = 1;
    else if (mode && strcmp(mode, "never") == 0)
        g_color = 0;
    else
        g_color = isatty(2) && !env_flag("NO_COLOR");
}

/*
 * Messages are first written into g_out with small style markers: a byte with
 * value 1 followed by a letter (the C_* macros). emit() then writes the text
 * twice: to the terminal (stderr) with colors if they are on, and, if --log
 * was given, as plain text to the log file.
 */
#define C_RED    "\001R"      /* bold red: the warning itself      */
#define C_GREEN  "\001G"      /* bold green: fixes, "no problems"  */
#define C_YELLOW "\001Y"      /* notes                             */
#define C_GREY   "\001D"      /* details                           */
#define C_BOLD   "\001B"
#define C_OFF    "\001Z"      /* back to normal                    */

static char g_plain[OUT_SIZE];
static char g_colored[OUT_SIZE * 3];

static const char *ansi_code(char c)
{
    switch (c) {
    case 'R': return "\033[1;31m";
    case 'G': return "\033[1;32m";
    case 'Y': return "\033[33m";
    case 'D': return "\033[90m";
    case 'B': return "\033[1m";
    default:  return "\033[0m";
    }
}

static void write_all(int fd, const char *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w <= 0)
            return;
        buf += w;
        n   -= (size_t)w;
    }
}

/* Append bytes to a file given by name (used for the log and status files). */
static void append_file(const char *path, const char *buf, size_t n)
{
    if (!path || !path[0])
        return;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0)
        return;
    write_all(fd, buf, n);
    close(fd);
}

/* The caller holds g_print_mutex (it protects the static buffers). */
static void emit(const char *text, size_t n)
{
    load_config();
    size_t p = 0, c = 0;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\001' && i + 1 < n) {          /* a style marker */
            i++;
            if (g_color) {
                const char *a = ansi_code(text[i]);
                size_t al = strlen(a);
                if (c + al < sizeof g_colored) {
                    memcpy(g_colored + c, a, al);
                    c += al;
                }
            }
            continue;
        }
        if (p < sizeof g_plain)   g_plain[p++]   = text[i];
        if (c < sizeof g_colored) g_colored[c++] = text[i];
    }
    if (g_color)
        write_all(2, g_colored, c);
    else
        write_all(2, g_plain, p);
    append_file(g_logpath, g_plain, p);
}

/* Tell the launcher what happened: 'W' = a warning was printed,
 * 'S' = the summary was printed. */
static void status_note(char c)
{
    append_file(g_statuspath, &c, 1);
}

static void bprintf(size_t *len, const char *fmt, ...)
{
    if (*len >= OUT_SIZE - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_out + *len, OUT_SIZE - *len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *len += (size_t)n;
        if (*len > OUT_SIZE - 1)
            *len = OUT_SIZE - 1;
    }
}

/* Up to two "called from" lines for a saved call stack. Frames inside system
 * libraries (such as libc's start_thread) have no line info and are skipped. */
static void bprint_callers(size_t *len, void *const *stack, int depth)
{
    char more[600];
    int shown = 0;
    for (int f = 1; f < depth && shown < 2; f++) {
        int has_line;
        describe(stack[f], more, sizeof more, &has_line);
        if (has_line) {
            bprintf(len, C_GREY "          called from        %s" C_OFF "\n", more);
            shown++;
        }
    }
}

/* A little picture of a short cycle, for example
 *
 *     lock#1 --> lock#2 --> lock#3
 *        ^                     |
 *        +---------------------+
 */
static void bprint_picture(size_t *len, const Cycle *c)
{
    if (c->n < 2 || c->total > 6)
        return;                                /* only short cycles are drawn */

    char lab[6][24];
    int  start[6], col = 4;
    for (int i = 0; i < c->n; i++) {
        snprintf(lab[i], sizeof lab[i], "lock#%d", c->e[i].from + 1);
        start[i] = col;
        col += (int)strlen(lab[i]) + 5;        /* the label and " --> " */
    }
    int first = start[0] + (int)strlen(lab[0]) / 2;
    int last  = start[c->n - 1] + (int)strlen(lab[c->n - 1]) / 2;

    bprintf(len, C_RED "    ");
    for (int i = 0; i < c->n; i++)
        bprintf(len, "%s%s", lab[i], i < c->n - 1 ? " --> " : "");
    bprintf(len, "\n%*s^%*s|\n", first, "", last - first - 1, "");
    bprintf(len, "%*s+", first, "");
    for (int k = first + 1; k < last; k++)
        bprintf(len, "-");
    bprintf(len, "+" C_OFF "\n\n");
}

/* The caller holds g_print_mutex. */
static void write_out(size_t len)
{
    emit(g_out, len);
    status_note('W');

    g_warnings_printed++;
    if (g_warnings_printed == MAX_WARNINGS) {
        static const char msg[] =
            C_GREY "[LockSight] (warning limit reached; further problems are counted "
            "but not printed)" C_OFF "\n";
        emit(msg, sizeof msg - 1);
    }
}

/* ------------------------------------------------------------------ */
/* JSON Lines output (--json FILE)                                      */
/*                                                                      */
/* One JSON object per line, one line per event: potential_deadlock,    */
/* self_deadlock, note, strict_stop and summary. It is written next to  */
/* the human-readable text and independent of --quiet and colors. The   */
/* line is built in a fixed buffer; if an event would not fit, a short  */
/* (still valid) line saying "details_truncated" is written instead.    */
/* The caller holds g_print_mutex.                                      */
/* ------------------------------------------------------------------ */
#ifndef JSON_SIZE
#define JSON_SIZE 131072
#endif
static char   g_json[JSON_SIZE];
static size_t g_jlen;
static int    g_jover;

static void jprintf(const char *fmt, ...)
{
    if (g_jover)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_json + g_jlen, JSON_SIZE - g_jlen - 2, fmt, ap);   /* room for "}\n" */
    va_end(ap);
    if (n < 0 || (size_t)n >= JSON_SIZE - g_jlen - 2)
        g_jover = 1;
    else
        g_jlen += (size_t)n;
}

/* A JSON string, escaped. Bytes that are not valid UTF-8 become '?'. */
static void jstr(const char *t)
{
    jprintf("\"");
    for (const unsigned char *p = (const unsigned char *)(t ? t : ""); *p; ) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            jprintf("\\%c", c);
            p++;
        } else if (c < 0x20) {
            jprintf("\\u%04x", c);
            p++;
        } else if (c < 0x80) {
            jprintf("%c", c);
            p++;
        } else {
            int need = (c >= 0xF0 && c <= 0xF4) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC2 && c < 0xE0) ? 1 : -1;
            int ok = need > 0 && c <= 0xF4;
            for (int k = 1; ok && k <= need; k++)
                if ((p[k] & 0xC0) != 0x80)
                    ok = 0;
            if (!ok) {
                jprintf("?");
                p++;
            } else {
                for (int k = 0; k <= need; k++)
                    jprintf("%c", p[k]);
                p += need + 1;
            }
        }
    }
    jprintf("\"");
}

/* A source location as an object: {"file":..,"line":..,"function":..,"pc":".."}
 * (file/line/function are null when the program has no debug info). */
static void jloc(void *pc)
{
    char text[600], loc[300], func[300];
    int has_line = 0;
    describe(pc, text, sizeof text, &has_line);
    jprintf("{\"pc\":\"%p\",", pc);
    const char *paren = has_line ? strstr(text, " (") : NULL;
    if (paren) {
        size_t ll = (size_t)(paren - text);
        if (ll >= sizeof loc) ll = sizeof loc - 1;
        memcpy(loc, text, ll);
        loc[ll] = '\0';
        snprintf(func, sizeof func, "%s", paren + 2);
        size_t fl = strlen(func);
        if (fl > 0 && func[fl - 1] == ')')
            func[fl - 1] = '\0';
        char *colon = strrchr(loc, ':');
        jprintf("\"file\":");
        if (colon) {
            *colon = '\0';
            jstr(loc);
            int ln = atoi(colon + 1);
            if (ln > 0) jprintf(",\"line\":%d", ln);
            else        jprintf(",\"line\":null");
        }
        else       { jstr(loc);                 jprintf(",\"line\":null"); }
        jprintf(",\"function\":");
        jstr(func);
    } else {
        jprintf("\"file\":null,\"line\":null,\"function\":null,\"text\":");
        jstr(text);
    }
    jprintf("}");
}

/* Up to three "called from" locations (frames with line info), as an array. */
static void jcallers(void *const *stack, int depth)
{
    char dummy[600];
    int shown = 0;
    jprintf("[");
    for (int f = 1; f < depth && shown < 3; f++) {
        int has_line;
        describe(stack[f], dummy, sizeof dummy, &has_line);
        if (has_line) {
            jprintf("%s", shown ? "," : "");
            jloc(stack[f]);
            shown++;
        }
    }
    jprintf("]");
}

static const char *json_mode(int kind, int mode)
{
    return (kind == KIND_RWLOCK && mode == MODE_S) ? "shared" : "exclusive";
}

/* Start an event line: {"schema":..,"event":..,"pid":..,"time":..  (more follows) */
static void json_begin(const char *event)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    g_jlen  = 0;
    g_jover = 0;
    jprintf("{\"schema\":\"locksight/1\",\"event\":\"%s\",\"pid\":%d,"
            "\"time\":\"%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ\"",
            event, (int)getpid(), tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);
}

/* Finish and write the line. 'fallback_tail' is what to write instead if the
 * event did not fit (it must be valid JSON that closes the object). */
static void json_end(const char *fallback_tail)
{
    if (!g_jsonpath[0])
        return;
    if (g_jover) {
        /* Keep schema/event/pid/time and the event number, drop the rest. */
        char *num = strstr(g_json, ",\"number\":");
        size_t keep = 0;
        if (num) {
            keep = (size_t)(num - g_json) + 10;
            while (g_json[keep] >= '0' && g_json[keep] <= '9')
                keep++;
        }
        g_jlen  = keep;
        g_jover = 0;
        jprintf("%s", fallback_tail);
    }
    g_json[g_jlen++] = '}';
    g_json[g_jlen++] = '\n';
    append_file(g_jsonpath, g_json, g_jlen);
}

static void json_lock(int id1, int kind, void *addr, void *first_pc)
{
    jprintf("{\"id\":%d,\"kind\":\"%s\",\"address\":\"%p\",\"first_used_at\":", id1,
            kind == KIND_RWLOCK ? "rwlock" : kind == KIND_SEM ? "semaphore" : "mutex", addr);
    jloc(first_pc);
    jprintf("}");
}

static void json_cycle(const Cycle *c)
{
    if (!g_jsonpath[0])
        return;
    json_begin("potential_deadlock");
    jprintf(",\"number\":%d,\"cycle_length\":%d,\"truncated\":%s,\"locks\":[",
            c->number, c->total, c->total > c->n ? "true" : "false");
    for (int i = 0; i < c->n; i++) {
        jprintf("%s", i ? "," : "");
        json_lock(c->e[i].from + 1, c->l[i].kind, c->l[i].addr, c->l[i].first_pc);
    }
    jprintf("],\"steps\":[");
    for (int i = 0; i < c->n; i++) {
        const Edge *e = &c->e[i];
        jprintf("%s{\"thread\":%d,\"holds\":{\"lock\":%d,\"mode\":\"%s\",\"acquired_at\":",
                i ? "," : "", e->tid, e->from + 1, json_mode(e->hkind, e->hmode));
        jloc(e->from_pc);
        jprintf("},\"requests\":{\"lock\":%d,\"mode\":\"%s\",\"requested_at\":",
                e->to + 1, json_mode(e->rkind, e->rmode));
        jloc(e->to_depth > 0 ? e->to_stack[0] : NULL);
        jprintf(",\"called_from\":");
        jcallers(e->to_stack, e->to_depth);
        jprintf("}}");
    }
    jprintf("],\"fix\":\"make every thread take these locks in the same order\"");
    json_end(",\"details_truncated\":true");
}

static void json_self(const SelfDeadlock *s)
{
    if (!g_jsonpath[0])
        return;
    json_begin("self_deadlock");
    jprintf(",\"number\":%d,\"thread\":%d,\"lock\":", s->number, s->tid);
    json_lock(s->id + 1, s->kind, s->lock.addr, s->lock.first_pc);
    jprintf(",\"held_at\":");
    jloc(s->held_pc);
    jprintf(",\"requested_at\":");
    jloc(s->req_depth > 0 ? s->req_stack[0] : NULL);
    jprintf(",\"called_from\":");
    jcallers(s->req_stack, s->req_depth);
    jprintf(",\"fix\":\"%s\"", s->kind == KIND_RWLOCK
            ? "release the read lock first, or take the write lock from the start"
            : "unlock before locking again, or use a recursive mutex if intended");
    json_end(",\"details_truncated\":true");
}

static const char *kind_name(int kind)
{
    return kind == KIND_RWLOCK ? "rwlock" : kind == KIND_SEM ? "semaphore" : "mutex";
}

/* " (read)" / " (write)" for rwlocks, nothing for other locks. */
static const char *mode_word(int kind, int mode)
{
    if (kind != KIND_RWLOCK)
        return "";
    return mode == MODE_S ? " (read)" : " (write)";
}

static void print_cycle(const Cycle *c)
{
    char where[600];
    size_t len = 0;

    real_lock(&g_print_mutex);                 /* one warning at a time */

    /* Look up every address we are about to print in ONE go (see Part 1). */
    void *want[MAX_CYCLE * (MAX_FRAMES + 2)];
    int nw = 0;
    for (int i = 0; i < c->n; i++) {
        want[nw++] = c->l[i].first_pc;
        want[nw++] = c->e[i].from_pc;
        for (int f = 0; f < c->e[i].to_depth; f++)
            want[nw++] = c->e[i].to_stack[f];
    }
    resolve_batch(want, nw);

    bprintf(&len, "\n" C_RED "[LockSight] POTENTIAL DEADLOCK #%d detected" C_OFF "\n\n",
            c->number);

    /* 1. The cycle as a chain of lock names, a picture, and the threads involved. */
    bprintf(&len, "  Lock order cycle:  " C_RED);
    for (int i = 0; i < c->n; i++)
        bprintf(&len, "lock#%d -> ", c->e[i].from + 1);
    if (c->total > c->n)
        bprintf(&len, "... (%d more) -> ", c->total - c->n);
    bprintf(&len, "lock#%d" C_OFF "\n\n", c->e[0].from + 1);

    bprint_picture(&len, c);

    bprintf(&len, "  Threads involved:  ");
    for (int i = 0; i < c->n; i++)
        bprintf(&len, "%sT%d", i ? ", " : "", c->e[i].tid);
    if (c->total > c->n)
        bprintf(&len, ", ...");
    bprintf(&len, "\n" C_GREY "  These threads hold no lock in common, so nothing stops them from\n"
                  "  blocking each other forever." C_OFF "\n\n");

    /* 2. Where each lock in the cycle was first used. */
    bprintf(&len, "  Locks involved:\n");
    for (int i = 0; i < c->n; i++) {
        describe(c->l[i].first_pc, where, sizeof where, NULL);
        bprintf(&len, C_GREY "    lock#%-3d %p  %-9s first used at %s" C_OFF "\n",
                c->e[i].from + 1, c->l[i].addr, kind_name(c->l[i].kind), where);
    }
    bprintf(&len, "\n");

    /* 3. One block per edge: which thread took which lock while holding which. */
    for (int i = 0; i < c->n; i++) {
        const Edge *e = &c->e[i];
        bprintf(&len, C_BOLD "  Thread T%d took lock#%d%s while holding lock#%d%s" C_OFF "\n",
                e->tid, e->to + 1, mode_word(e->rkind, e->rmode),
                e->from + 1, mode_word(e->hkind, e->hmode));

        describe(e->from_pc, where, sizeof where, NULL);
        bprintf(&len, C_GREY "      lock#%d was acquired at   %s" C_OFF "\n", e->from + 1, where);

        describe(e->to_depth > 0 ? e->to_stack[0] : NULL, where, sizeof where, NULL);
        bprintf(&len, C_GREY "      lock#%d was requested at  %s" C_OFF "\n", e->to + 1, where);

        bprint_callers(&len, e->to_stack, e->to_depth);
        bprintf(&len, "\n");
    }

    int has_rw = 0, has_sem = 0;
    for (int i = 0; i < c->n; i++) {
        if (c->e[i].hkind == KIND_RWLOCK || c->e[i].rkind == KIND_RWLOCK) has_rw = 1;
        if (c->e[i].hkind == KIND_SEM    || c->e[i].rkind == KIND_SEM)    has_sem = 1;
    }
    bprintf(&len, "  " C_GREEN "Fix:" C_OFF " make every thread take these locks in the same order.\n");
    json_cycle(c);
    if (has_rw)
        bprintf(&len, C_GREY "  (Read locks only block writers. Every step of this cycle pairs a request\n"
                      "  with a hold that really conflicts: write/write or read/write.)" C_OFF "\n");
    if (has_sem)
        bprintf(&len, C_GREY "  (A semaphore counts as a lock here because it was created with an\n"
                      "  initial value of 1. If it is really used for signaling, ignore its part.)"
                      C_OFF "\n");
    write_out(len);
    real_unlock(&g_print_mutex);
}

static void print_self_deadlock(const SelfDeadlock *s)
{
    char where[600];
    size_t len = 0;

    real_lock(&g_print_mutex);

    void *want[MAX_FRAMES + 2];
    int nw = 0;
    want[nw++] = s->lock.first_pc;
    want[nw++] = s->held_pc;
    for (int f = 0; f < s->req_depth; f++)
        want[nw++] = s->req_stack[f];
    resolve_batch(want, nw);

    bprintf(&len, "\n" C_RED "[LockSight] SELF-DEADLOCK #%d detected" C_OFF "\n\n", s->number);
    if (s->kind == KIND_RWLOCK)
        bprintf(&len, "  Thread T%d asks for a WRITE lock on lock#%d, but it already holds a READ lock on it.\n"
                      C_GREY "  A read lock cannot be upgraded to a write lock: the write lock waits until\n"
                      "  all readers are gone, and this thread is one of them." C_OFF "\n\n",
                s->tid, s->id + 1);
    else
        bprintf(&len, "  Thread T%d asks for lock#%d, but it already holds that lock.\n"
                      C_GREY "  A normal (non-recursive) mutex cannot be locked twice by the same\n"
                      "  thread, so this thread will wait for itself forever." C_OFF "\n\n",
                s->tid, s->id + 1);

    describe(s->lock.first_pc, where, sizeof where, NULL);
    bprintf(&len, C_GREY "    lock#%-3d %p  first used at %s" C_OFF "\n\n",
            s->id + 1, s->lock.addr, where);

    describe(s->held_pc, where, sizeof where, NULL);
    bprintf(&len, C_BOLD "  Thread T%d" C_OFF "\n", s->tid);
    bprintf(&len, C_GREY "      lock#%d was acquired at        %s" C_OFF "\n", s->id + 1, where);
    describe(s->req_depth > 0 ? s->req_stack[0] : NULL, where, sizeof where, NULL);
    bprintf(&len, C_GREY "      lock#%d is requested again at  %s" C_OFF "\n", s->id + 1, where);
    bprint_callers(&len, s->req_stack, s->req_depth);

    if (s->kind == KIND_RWLOCK)
        bprintf(&len, "\n  " C_GREEN "Fix:" C_OFF " release the read lock first, or take the write lock "
                      "from the start.\n");
    else
        bprintf(&len, "\n  " C_GREEN "Fix:" C_OFF " unlock it before locking again, or create the mutex with\n"
                      "  PTHREAD_MUTEX_RECURSIVE if locking it twice is intended.\n");
    json_self(s);
    write_out(len);
    real_unlock(&g_print_mutex);
}

/* The summary block (not printed with --quiet). */
static void print_summary(void)
{
    load_config();
    if (g_quiet && !g_jsonpath[0])
        return;

    size_t len = 0;
    real_lock(&g_print_mutex);

    if (g_jsonpath[0]) {
        json_begin("summary");
        jprintf(",\"threads\":%d,\"locks_seen\":%d,\"locks_tracked\":%d,"
                "\"lock_order_edges\":%d,\"potential_deadlocks\":%d,\"self_deadlocks\":%d,"
                "\"semaphore_notes\":%d,\"table_limit_locks_dropped\":%d,"
                "\"table_limit_edges_dropped\":%d,\"search_gave_up\":%d,\"result\":\"%s\"",
                g_nthreads, g_nlocks_seen, g_nlocks_live, g_nedges_seen, g_ncycles, g_nself,
                g_nnotes, g_dropped_locks, g_dropped_edges, g_budget_hits,
                (g_ncycles + g_nself) > 0 ? "problems_found" : "no_problems");
        json_end("");
    }
    if (g_quiet) {
        real_unlock(&g_print_mutex);
        return;
    }

    int problems = g_ncycles + g_nself;
    bprintf(&len, "\n" C_BOLD "[LockSight] Summary" C_OFF "\n");
    bprintf(&len, "  threads that used locks   %d\n", g_nthreads);
    bprintf(&len, "  locks seen                %d  (%d still tracked)\n",
            g_nlocks_seen, g_nlocks_live);
    bprintf(&len, "  lock-order edges          %d\n", g_nedges_seen);
    bprintf(&len, "  potential deadlocks       %d\n", g_ncycles);
    bprintf(&len, "  self-deadlocks            %d\n", g_nself);
    if (problems > 0)
        bprintf(&len, "  result                    " C_RED "PROBLEMS FOUND" C_OFF
                      " (see the warnings above)\n");
    else
        bprintf(&len, "  result                    " C_GREEN "NO PROBLEMS FOUND" C_OFF "\n");

    if (g_dropped_locks || g_dropped_edges)
        bprintf(&len, C_YELLOW "  note: table limits reached (%d lock(s), %d edge(s) not tracked)"
                      C_OFF "\n", g_dropped_locks, g_dropped_edges);
    if (g_nnotes)
        bprintf(&len, C_YELLOW "  note: %d semaphore(s) were used for signaling and are not tracked"
                      C_OFF "\n", g_nnotes);
    if (g_budget_hits)
        bprintf(&len, C_YELLOW "  note: the cycle search gave up %d time(s) (very large lock graph)"
                      C_OFF "\n", g_budget_hits);

    emit(g_out, len);
    status_note('S');
    real_unlock(&g_print_mutex);
}

/* --strict: stop the program right now, because of a problem just reported. */
static void strict_stop(void)
{
    alarm(2);                       /* safety net: never hang while flushing */
    fflush(NULL);                   /* let the program's own output appear   */

    size_t len = 0;
    real_lock(&g_print_mutex);
    bprintf(&len, "\n" C_RED "[LockSight] --strict: stopping the program now because of "
                  "the problem reported above." C_OFF "\n");
    if (g_jsonpath[0]) {
        json_begin("strict_stop");
        jprintf(",\"exit_status\":3");
        json_end("");
    }
    emit(g_out, len);
    real_unlock(&g_print_mutex);

    print_summary();
    _exit(3);
}

/* ================================================================== */
/* Part 3: the lock-order graph (always called with g_mutex held)       */
/* ================================================================== */

/* ---- Locks: a hash table from address to lock id ---- */

static unsigned hash_addr(void *p)
{
    uintptr_t x = (uintptr_t)p >> 3;
    x ^= x >> 17;
    x *= 0x9E3779B1u;
    return (unsigned)(x % HASH_SIZE);
}

/* The id of the mutex at 'addr', or -1 if we do not know it. */
static int find_lock(void *addr)
{
    unsigned h = hash_addr(addr);
    for (int n = 0; n < HASH_SIZE; n++) {
        int s = g_slot[h];
        if (s == 0)
            return -1;
        if (g_locks[s - 1].addr == addr)
            return s - 1;
        h = (h + 1) % HASH_SIZE;
    }
    return -1;
}

/* Find the id of the mutex at 'addr', adding it if we have not seen it yet. */
static int lookup_or_add_lock(void *addr, void *pc, int kind)
{
    int id = find_lock(addr);
    if (id >= 0)
        return id;

    if (g_nlock_free > 0)
        id = g_lock_free[--g_nlock_free];        /* reuse the id of a forgotten lock */
    else if (g_nlocks_hw < MAX_LOCKS)
        id = g_nlocks_hw++;
    else {
        g_dropped_locks++;
        return -1;
    }
    g_locks[id].addr     = addr;
    g_locks[id].first_pc = pc;
    g_locks[id].kind     = kind;
    g_ord[id]            = ++g_next_ord;     /* a new lock goes last in the ordering */

    unsigned h = hash_addr(addr);
    while (g_slot[h] != 0)
        h = (h + 1) % HASH_SIZE;
    g_slot[h] = id + 1;

    g_nlocks_live++;
    g_nlocks_seen++;
    return id;
}

/* Remove the entry for 'addr' from the hash table. With linear probing the
 * entries behind the removed one must be shifted back so that later lookups
 * still find them ("backward-shift deletion"). */
static void hash_remove(void *addr)
{
    unsigned i = hash_addr(addr);
    for (;;) {
        int s = g_slot[i];
        if (s == 0)
            return;                              /* not in the table */
        if (g_locks[s - 1].addr == addr)
            break;
        i = (i + 1) % HASH_SIZE;
    }
    unsigned j = i;
    for (;;) {
        g_slot[i] = 0;
        for (;;) {
            j = (j + 1) % HASH_SIZE;
            if (g_slot[j] == 0)
                return;
            unsigned k = hash_addr(g_locks[g_slot[j] - 1].addr);   /* ideal slot */
            int stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (!stays)
                break;
        }
        g_slot[i] = g_slot[j];
        i = j;
    }
}

/* ---- Edges: a pool of records with free-list reuse ---- */

static int alloc_edge(void)
{
    if (g_nedge_free > 0)
        return g_edge_free[--g_nedge_free];
    if (g_nedges_hw < MAX_EDGES)
        return g_nedges_hw++;
    g_dropped_edges++;
    return -1;
}

static void free_edge(int ei)
{
    g_edge_free[g_nedge_free++] = ei;
}

/* Edges are kept in three structures, all updated in O(1):
 *   - the list of edges leaving 'from' (doubly linked),
 *   - the list of edges entering 'to'  (doubly linked),
 *   - a hash index on (from, to), so "do we already have this pair?" does not
 *     have to walk a long list when one lock (a "hub") has very many edges. */

static unsigned pair_bucket(int from, int to)
{
    uint64_t x = ((uint64_t)(unsigned)from << 32) | (unsigned)to;
    x *= 0x9E3779B97F4A7C15ULL;
    return (unsigned)((x >> 32) % PAIR_HASH);
}

static void link_edge(int ei)
{
    Edge *e = &g_edges[ei];

    e->prev_out = 0;
    e->next_out = g_first_out[e->from];
    if (e->next_out)
        g_edges[e->next_out - 1].prev_out = ei + 1;
    g_first_out[e->from] = ei + 1;

    e->prev_in = 0;
    e->next_in = g_first_in[e->to];
    if (e->next_in)
        g_edges[e->next_in - 1].prev_in = ei + 1;
    g_first_in[e->to] = ei + 1;

    unsigned b = pair_bucket(e->from, e->to);
    e->next_pair = g_pair_head[b];
    g_pair_head[b] = ei + 1;
}

static void unlink_out(int from, int ei)
{
    Edge *e = &g_edges[ei];
    if (e->prev_out)
        g_edges[e->prev_out - 1].next_out = e->next_out;
    else
        g_first_out[from] = e->next_out;
    if (e->next_out)
        g_edges[e->next_out - 1].prev_out = e->prev_out;
}

static void unlink_in(int to, int ei)
{
    Edge *e = &g_edges[ei];
    if (e->prev_in)
        g_edges[e->prev_in - 1].next_in = e->next_in;
    else
        g_first_in[to] = e->next_in;
    if (e->next_in)
        g_edges[e->next_in - 1].prev_in = e->prev_in;
}

static void unlink_pair(int ei)
{
    int *p = &g_pair_head[pair_bucket(g_edges[ei].from, g_edges[ei].to)];
    while (*p != 0) {
        if (*p - 1 == ei) {
            *p = g_edges[ei].next_pair;
            return;
        }
        p = &g_edges[*p - 1].next_pair;
    }
}

/* ---- Lock ordering (keeps most cycle searches from being necessary) ----
 *
 * Looking for a cycle every time an edge is added is expensive in a big graph
 * (thousands of locks, a hundred thousand edges). But in a program WITHOUT a
 * lock-order problem the plain graph has no cycle at all, and then its locks can
 * be put in a line so that every edge points forward. We keep such a line
 * (g_ord) up to date as edges arrive (the Pearce-Kelly algorithm): a new edge
 * that already points forward cannot close a loop, so nothing has to be
 * searched. Only an edge that points backward needs work: the nodes between its
 * two ends are re-sorted, or, if it really closes a loop in the plain graph, it
 * is flagged "back" and set aside (the line only covers the other edges).
 * A loop must contain at least one such set-aside edge, which is what
 * needs_search() uses to decide whether the full search has to run. This only
 * SKIPS searches that cannot find anything; it never changes what is found. */

static unsigned g_pkmark[MAX_LOCKS];
static unsigned g_pkgen;
static int      g_pkstack[MAX_LOCKS];
static int      g_pkfwd[MAX_LOCKS], g_pkbwd[MAX_LOCKS];
static uint64_t g_pkpool[MAX_LOCKS];

static void back_forget(int ei)
{
    if (!g_edges[ei].back)
        return;
    for (int i = 0; i < g_nback; i++) {
        if (g_back[i] == ei) {
            g_back[i] = g_back[--g_nback];
            break;
        }
    }
    g_edges[ei].back = 0;
}

static int cmp_ord(const void *a, const void *b)
{
    uint64_t x = g_ord[*(const int *)a], y = g_ord[*(const int *)b];
    return x < y ? -1 : x > y;
}

/* The edge 'ei' (u -> v) was just linked in. Keep the ordering valid.
 * Returns 1 if the edge closes a loop in the plain graph (it is then set aside
 * as a "back" edge), 0 otherwise. */
static int order_add_edge(int ei)
{
    Edge *e = &g_edges[ei];
    int u = e->from, v = e->to;
    e->back = 0;
    if (g_ord[u] < g_ord[v])
        return 0;                               /* already points forward */

    uint64_t lb = g_ord[v], ub = g_ord[u];
    g_pkgen += 2;
    unsigned gf = g_pkgen, gb = g_pkgen + 1;

    /* forward from v, only through nodes before u in the line */
    int nf = 0, sp = 0;
    g_pkstack[sp++] = v;
    g_pkmark[v] = gf;
    while (sp > 0) {
        int x = g_pkstack[--sp];
        g_pkfwd[nf++] = x;
        for (int f = g_first_out[x]; f != 0; f = g_edges[f - 1].next_out) {
            const Edge *fe = &g_edges[f - 1];
            if (fe->back || f - 1 == ei)
                continue;
            int y = fe->to;
            if (y == u)
                goto closes_loop;
            if (g_ord[y] < ub && g_pkmark[y] != gf) {
                g_pkmark[y] = gf;
                g_pkstack[sp++] = y;
            }
        }
    }

    /* backward from u, only through nodes after v in the line */
    int nb = 0;
    sp = 0;
    g_pkstack[sp++] = u;
    g_pkmark[u] = gb;
    while (sp > 0) {
        int x = g_pkstack[--sp];
        g_pkbwd[nb++] = x;
        for (int f = g_first_in[x]; f != 0; f = g_edges[f - 1].next_in) {
            const Edge *fe = &g_edges[f - 1];
            if (fe->back || f - 1 == ei)
                continue;
            int y = fe->from;
            if (g_ord[y] > lb && g_pkmark[y] != gb) {
                g_pkmark[y] = gb;
                g_pkstack[sp++] = y;
            }
        }
    }

    /* Re-sort: everything that leads to u goes before everything reachable from
     * v, using the same set of positions. */
    qsort(g_pkfwd, (size_t)nf, sizeof(int), cmp_ord);
    qsort(g_pkbwd, (size_t)nb, sizeof(int), cmp_ord);
    {
        int i = 0, j = 0, k = 0;
        while (i < nb || j < nf) {
            if (j >= nf || (i < nb && g_ord[g_pkbwd[i]] < g_ord[g_pkfwd[j]]))
                g_pkpool[k++] = g_ord[g_pkbwd[i++]];
            else
                g_pkpool[k++] = g_ord[g_pkfwd[j++]];
        }
        k = 0;
        for (int a = 0; a < nb; a++)
            g_ord[g_pkbwd[a]] = g_pkpool[k++];
        for (int a = 0; a < nf; a++)
            g_ord[g_pkfwd[a]] = g_pkpool[k++];
    }
    return 0;

closes_loop:
    e->back = 1;
    if (g_nback < MAX_BACK)
        g_back[g_nback++] = ei;
    else
        g_back_overflow = 1;
    return 1;
}

/* Could the edge 'ei' be part of a loop of the plain graph? If not, the full
 * search has nothing to find. */
static int needs_search(int ei)
{
    const Edge *e = &g_edges[ei];
    if (e->back || g_back_overflow)
        return 1;
    if (g_nback == 0)
        return 0;                       /* every edge points forward: no loops at all */
    /* A loop through this edge needs a set-aside edge reachable from e->to and
     * one that leads back to e->from (possibly two different ones). */
    int any_out = 0, any_in = 0;
    for (int i = 0; i < g_nback; i++) {
        const Edge *b = &g_edges[g_back[i]];
        if (g_ord[b->from] >= g_ord[e->to])   any_out = 1;
        if (g_ord[b->to]   <= g_ord[e->from]) any_in  = 1;
    }
    return any_out && any_in;
}

/* Forget a lock completely: all edges leaving or entering it, its address,
 * and its id (which can then be given to a new lock). */
static void drop_lock(int id)
{
    for (int e = g_first_out[id]; e != 0; ) {
        int ei = e - 1, nxt = g_edges[ei].next_out;
        unlink_in(g_edges[ei].to, ei);
        unlink_pair(ei);
        back_forget(ei);
        free_edge(ei);
        e = nxt;
    }
    g_first_out[id] = 0;

    for (int e = g_first_in[id]; e != 0; ) {
        int ei = e - 1, nxt = g_edges[ei].next_in;
        unlink_out(g_edges[ei].from, ei);
        unlink_pair(ei);
        back_forget(ei);
        free_edge(ei);
        e = nxt;
    }
    g_first_in[id] = 0;

    hash_remove(g_locks[id].addr);
    g_locks[id].addr = NULL;
    g_locks[id].gen++;               /* every thread's memory of holding it is now void */
    g_lock_free[g_nlock_free++] = id;
    g_nlocks_live--;
}

/* Save the call stack of a lock request. Frame 0 is the exact place the
 * program called pthread_mutex_lock (we know it from __builtin_return_address);
 * the frames after it come from backtrace(). Returns the number of frames. */
static int capture_stack(void *pc, void **out, int cap)
{
    int depth = 1;
    out[0] = pc;
    void *frames[MAX_FRAMES + 8];
    int n = backtrace(frames, MAX_FRAMES + 8);
    for (int i = 0; i < n; i++) {
        if (frames[i] == pc) {
            for (int j = i + 1; j < n && depth < cap; j++)
                out[depth++] = frames[j];
            break;
        }
    }
    return depth;
}

/* Note that the current thread asked for 'to' while holding 'from'.
 *   - First time for this thread and pair: a new record is made.
 *   - Seen before: we keep only the locks that were held EVERY time (a lock
 *     that was sometimes missing cannot be trusted as a gate).
 * Returns the index of the record if something NEW was learned (so a cycle
 * check is worthwhile), or -1 if nothing changed. */
static int note_dependency(int from, int to, const uint64_t *held, const uint64_t *hx,
                           int hmode, int rmode, void *from_pc, void *to_pc)
{
    int same_pair = 0;
    for (int ei = g_pair_head[pair_bucket(from, to)]; ei != 0; ei = g_edges[ei - 1].next_pair) {
        Edge *e = &g_edges[ei - 1];
        if (e->from != from || e->to != to || e->hmode != hmode || e->rmode != rmode)
            continue;
        same_pair++;
        if (e->tid == t_tid) {
            int changed = 0;
            for (int w = 0; w < GATE_WORDS; w++) {
                uint64_t now = e->held[w] & held[w];
                uint64_t nx  = e->hx[w] & hx[w];
                if (now != e->held[w] || nx != e->hx[w]) {
                    e->held[w] = now;
                    e->hx[w]   = nx;
                    changed = 1;
                }
            }
            return changed ? ei - 1 : -1;
        }
    }
    if (same_pair >= MAX_PER_EDGE)
        return -1;                      /* enough threads remembered for this pair */

    int ei = alloc_edge();
    if (ei < 0)
        return -1;
    Edge *e = &g_edges[ei];
    e->from = from;
    e->to   = to;
    e->tid  = t_tid;
    e->hmode = (unsigned char)hmode;
    e->rmode = (unsigned char)rmode;
    e->hkind = (unsigned char)g_locks[from].kind;
    e->rkind = (unsigned char)g_locks[to].kind;
    memcpy(e->held, held, sizeof e->held);
    memcpy(e->hx, hx, sizeof e->hx);
    e->from_pc  = from_pc;
    link_edge(ei);
    order_add_edge(ei);
    e->to_depth = capture_stack(to_pc, e->to_stack, MAX_FRAMES);
    g_nedges_seen++;
    return ei;
}

/* ---- Finding a dangerous cycle ---- */

/* Can records a and b be two links of the same real deadlock? They must come
 * from different threads, and the threads must be able to hold their locks AT
 * THE SAME TIME. That fails if both hold the same lock and at least one of them
 * holds it exclusively (a "gate" lock). Two readers of the same rwlock do not
 * keep each other out, so a shared lock is not a gate between readers. */
static int compatible(const Edge *a, const Edge *b)
{
    if (a->tid == b->tid)
        return 0;
    for (int w = 0; w < GATE_WORDS; w++)
        if ((a->held[w] & b->hx[w]) || (a->hx[w] & b->held[w]))
            return 0;                   /* a common "gate" lock protects both */
    return 1;
}

/* Depth-first search: is there a chain of records from 'cur' back to 'target'
 * in which every record is compatible with all the others? The records of the
 * chain so far are in g_chosen[0 .. g_depth-1]. */
static int search(int cur, int target)
{
    if (g_depth >= MAX_DEPTH)
        return 0;

    for (int ei = g_first_out[cur]; ei != 0; ei = g_edges[ei - 1].next_out) {
        if (--g_budget < 0)                      /* every edge looked at costs 1 */
            return 0;
        const Edge *e = &g_edges[ei - 1];

        int ok = 1;
        for (int k = 0; k < g_depth; k++) {
            if (!compatible(&g_edges[g_chosen[k]], e)) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;

        /* Link to the previous record: what the previous thread REQUESTS must
         * clash with how this thread HOLDS the same lock (a writer waits for a
         * reader, but a reader does not wait for another reader). */
        if (!conflicts(g_edges[g_chosen[g_depth - 1]].rmode, e->hmode))
            continue;

        if (e->to == target) {                   /* the loop is closed */
            if (!conflicts(e->rmode, g_edges[g_chosen[0]].hmode))
                continue;
            g_chosen[g_depth++] = ei - 1;
            return 1;
        }
        if (g_mark[e->to] == g_gen)              /* already on the current path */
            continue;

        g_mark[e->to] = g_gen;
        g_chosen[g_depth++] = ei - 1;
        if (search(e->to, target))
            return 1;
        g_depth--;
        g_mark[e->to] = 0;
    }
    return 0;
}

/* Does the record e0 (from -> to) close a dangerous cycle? */
static int find_cycle(int e0)
{
    if (!needs_search(e0))
        return 0;                                /* cannot be part of any loop */
    if (g_search_total >= SEARCH_TOTAL) {        /* this process has searched enough */
        g_budget_hits++;
        return 0;
    }
    const Edge *first = &g_edges[e0];
    g_gen++;
    if (g_gen == 0)
        g_gen = 1;
    g_mark[first->to] = g_gen;
    g_chosen[0] = e0;
    g_depth     = 1;
    g_budget    = SEARCH_BUDGET;

    int found = search(first->to, first->from);
    g_search_total += SEARCH_BUDGET - (g_budget < 0 ? 0 : g_budget);
    if (!found && g_budget < 0)
        g_budget_hits++;
    g_mark[first->to] = 0;
    return found;
}

/* ================================================================== */
/* Part 4: what happens just before a lock is taken                     */
/* ================================================================== */

/* Returns the lock id (or -1) and the lock's generation in *gen_out.
 * kind/mode say what is being asked for (see KIND_* and MODE_*).
 * If a new problem appears, it is printed. */
static int on_before_lock(void *addr, void *pc, int kind, int mode, unsigned *gen_out)
{
    Cycle        cyc;
    SelfDeadlock sd;
    int          have_self = 0;
    cyc.n = 0;
    *gen_out = 0;

    if (t_tid == 0)
        t_tid = __atomic_add_fetch(&g_nthreads, 1, __ATOMIC_RELAXED);

    real_lock(&g_mutex);

    /* The locks this thread holds right now (used to find gate locks).
     * An entry whose lock was flushed since (a hand-off to another thread, a
     * semaphore posted by someone else, or the address re-initialised) is
     * stale: it is dropped here instead of being believed. */
    uint64_t held[GATE_WORDS], hx[GATE_WORDS];
    memset(held, 0, sizeof held);
    memset(hx, 0, sizeof hx);
    int keep = 0;
    for (int i = 0; i < t_nheld; i++) {
        int hid = t_held[i].id;
        if (hid < 0 || g_locks[hid].addr != t_held[i].addr ||
            g_locks[hid].gen != t_held[i].gen)
            continue;
        t_held[keep++] = t_held[i];
        if (hid < GATE_BITS) {
            held[hid >> 6] |= 1ULL << (hid & 63);
            if (t_held[i].mode == MODE_X)
                hx[hid >> 6] |= 1ULL << (hid & 63);
        }
    }
    t_nheld = keep;

    /* A semaphore is only tracked if sem_init made it known (see sem_init). */
    int id = (kind == KIND_SEM) ? find_lock(addr) : lookup_or_add_lock(addr, pc, kind);
    if (id >= 0) {
        *gen_out = g_locks[id].gen;
        for (int i = 0; i < t_nheld; i++) {
            int h = t_held[i].id;

            if (h == id) {
                /* The thread already holds this very lock. For a normal mutex
                 * that means it will wait for itself forever. Recursive and
                 * error-checking mutexes are allowed to be locked again. For
                 * an rwlock, only "read lock held, write lock requested" (an
                 * upgrade) hangs; a second read lock is fine, and a write lock
                 * that is requested twice makes glibc return EDEADLK instead
                 * of hanging. A semaphore that is waited on twice may simply
                 * be waiting for another thread's sem_post: no verdict. */
                int self = 0;
                if (kind == KIND_MUTEX) {
                    int mk = ((pthread_mutex_t *)addr)->__data.__kind & 3;
                    self = (mk == PTHREAD_MUTEX_NORMAL || mk == PTHREAD_MUTEX_ADAPTIVE_NP);
                } else if (kind == KIND_RWLOCK) {
                    self = (t_held[i].mode == MODE_S && mode == MODE_X);
                }
                if (self && !have_self) {
                    have_self       = 1;
                    sd.lock         = g_locks[id];
                    sd.held_pc      = t_held[i].pc;
                    sd.req_depth    = capture_stack(pc, sd.req_stack, MAX_FRAMES);
                    sd.id           = id;
                    sd.tid          = t_tid;
                    sd.number       = ++g_nself;
                    sd.kind         = kind;
                }
                continue;
            }

            int e0 = note_dependency(h, id, held, hx, t_held[i].mode, mode,
                                     t_held[i].pc, pc);
            if (e0 >= 0 && cyc.n == 0 && find_cycle(e0)) {
                g_ncycles++;
                cyc.total  = g_depth;
                cyc.number = g_ncycles;
                cyc.n      = g_depth < MAX_CYCLE ? g_depth : MAX_CYCLE;
                for (int k = 0; k < cyc.n; k++) {
                    cyc.e[k] = g_edges[g_chosen[k]];
                    cyc.l[k] = g_locks[cyc.e[k].from];
                }
            }
        }
    }
    real_unlock(&g_mutex);

    int reported = 0;
    if (cyc.n > 0 && g_warnings_printed < MAX_WARNINGS) {
        print_cycle(&cyc);
        reported = 1;
    }
    if (have_self && g_warnings_printed < MAX_WARNINGS) {
        print_self_deadlock(&sd);
        reported = 1;
    }
    if (reported && g_strict)           /* --strict: do not let the program go on */
        strict_stop();
    return id;
}

/* ================================================================== */
/* Part 5: the functions the program actually calls                     */
/* ================================================================== */

/*
 * t_busy is the re-entrancy guard: while LockSight is doing its own work
 * (printing, running addr2line, ...) any lock calls made by that work are
 * passed straight to the real functions and are not tracked.
 */

/* ---- the calling thread's list of held locks ---- */

static void add_hold(void *obj, int id, unsigned gen, int mode, void *pc)
{
    if (id < 0 || t_nheld >= MAX_HELD)
        return;
    t_held[t_nheld].addr = obj;
    t_held[t_nheld].id   = id;
    t_held[t_nheld].mode = mode;
    t_held[t_nheld].gen  = gen;
    t_held[t_nheld].pc   = pc;
    t_nheld++;
}

/* Forget the newest hold of 'obj' (locks may be released in any order).
 * Returns 1 if this thread had it in its list. */
static int remove_hold(void *obj)
{
    for (int i = t_nheld - 1; i >= 0; i--) {
        if (t_held[i].addr == obj) {
            for (int j = i; j < t_nheld - 1; j++)
                t_held[j] = t_held[j + 1];
            t_nheld--;
            return 1;
        }
    }
    return 0;
}

/* A lock was taken without waiting (trylock, timed lock): the thread now HOLDS
 * it, so locks taken later while holding it must be recorded, but no edge is
 * added for the try itself (it cannot block forever). */
static void note_acquired_nowait(void *obj, int kind, int mode, void *pc)
{
    if (t_busy || t_nheld >= MAX_HELD)
        return;
    t_busy = 1;
    if (t_tid == 0)
        t_tid = __atomic_add_fetch(&g_nthreads, 1, __ATOMIC_RELAXED);
    real_lock(&g_mutex);
    int id = (kind == KIND_SEM) ? find_lock(obj) : lookup_or_add_lock(obj, pc, kind);
    unsigned gen = id >= 0 ? g_locks[id].gen : 0;
    real_unlock(&g_mutex);
    t_busy = 0;
    add_hold(obj, id, gen, mode, pc);
}

/* Somebody who does not hold 'obj' released it (a lock handed from one thread
 * to another). Whoever "held" it according to their own list is wrong now, so
 * raise the generation: every such entry becomes stale and is ignored. */
static void invalidate_holds(void *obj)
{
    t_busy = 1;
    real_lock(&g_mutex);
    int id = find_lock(obj);
    if (id >= 0)
        g_locks[id].gen++;
    real_unlock(&g_mutex);
    t_busy = 0;
}

/* The same memory address can hold a different lock later (a lock is
 * destroyed and another is created in the same place, or freed memory is
 * reused). Whatever we learned about the old one must not leak into the new
 * one, so on init and destroy we forget the address completely. */
static void forget_lock(void *addr)
{
    while (remove_hold(addr))                    /* also in this thread's own list */
        ;
    t_busy = 1;
    real_lock(&g_mutex);
    int id = find_lock(addr);
    if (id >= 0)
        drop_lock(id);
    real_unlock(&g_mutex);
    t_busy = 0;
}

/* ---- mutexes ---- */

int pthread_mutex_lock(pthread_mutex_t *m)
{
    init_real();
    if (t_busy)
        return real_lock(m);

    void *pc = __builtin_return_address(0);
    unsigned gen;
    t_busy = 1;
    int id = on_before_lock(m, pc, KIND_MUTEX, MODE_X, &gen);   /* edges + problems FIRST */
    t_busy = 0;

    int rc = real_lock(m);              /* ...then really take the lock           */
    if (rc == 0)
        add_hold(m, id, gen, MODE_X, pc);
    return rc;
}

/* trylock never waits, so it cannot be part of a deadlock by itself: we add
 * no edge for it. */
int pthread_mutex_trylock(pthread_mutex_t *m)
{
    init_real();
    int rc = real_trylock(m);
    if (rc == 0)
        note_acquired_nowait(m, KIND_MUTEX, MODE_X, __builtin_return_address(0));
    return rc;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *ts)
{
    init_real();
    int rc = real_timedlock(m, ts);     /* gives up after a while: like trylock */
    if (rc == 0)
        note_acquired_nowait(m, KIND_MUTEX, MODE_X, __builtin_return_address(0));
    return rc;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    init_real();
    int mine = remove_hold(m);
    if (!mine && !t_busy)
        invalidate_holds(m);            /* released by a thread that never took it */
    return real_unlock(m);
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    init_real();
    if (!t_busy)
        forget_lock(m);
    return real_init(m, attr);
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    init_real();
    if (!t_busy)
        forget_lock(m);
    return real_destroy(m);
}

/* ---- read-write locks ---- */

int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
    init_real();
    if (t_busy)
        return real_rdlock(l);
    void *pc = __builtin_return_address(0);
    unsigned gen;
    t_busy = 1;
    int id = on_before_lock(l, pc, KIND_RWLOCK, MODE_S, &gen);
    t_busy = 0;
    int rc = real_rdlock(l);
    if (rc == 0)
        add_hold(l, id, gen, MODE_S, pc);
    return rc;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
    init_real();
    if (t_busy)
        return real_wrlock(l);
    void *pc = __builtin_return_address(0);
    unsigned gen;
    t_busy = 1;
    int id = on_before_lock(l, pc, KIND_RWLOCK, MODE_X, &gen);
    t_busy = 0;
    int rc = real_wrlock(l);
    if (rc == 0)
        add_hold(l, id, gen, MODE_X, pc);
    return rc;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
    init_real();
    int rc = real_tryrdlock(l);
    if (rc == 0)
        note_acquired_nowait(l, KIND_RWLOCK, MODE_S, __builtin_return_address(0));
    return rc;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
    init_real();
    int rc = real_trywrlock(l);
    if (rc == 0)
        note_acquired_nowait(l, KIND_RWLOCK, MODE_X, __builtin_return_address(0));
    return rc;
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *ts)
{
    init_real();
    int rc = real_timedrdlock(l, ts);
    if (rc == 0)
        note_acquired_nowait(l, KIND_RWLOCK, MODE_S, __builtin_return_address(0));
    return rc;
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *ts)
{
    init_real();
    int rc = real_timedwrlock(l, ts);
    if (rc == 0)
        note_acquired_nowait(l, KIND_RWLOCK, MODE_X, __builtin_return_address(0));
    return rc;
}

/* One unlock call for both read and write locks. Several readers can hold the
 * same rwlock, so a release by a thread that does not hold it is ignored (we
 * cannot tell whose hold it was). */
int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    init_real();
    remove_hold(l);
    return real_rw_unlock(l);
}

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *attr)
{
    init_real();
    if (!t_busy)
        forget_lock(l);
    return real_rw_init(l, attr);
}

int pthread_rwlock_destroy(pthread_rwlock_t *l)
{
    init_real();
    if (!t_busy)
        forget_lock(l);
    return real_rw_destroy(l);
}

/* ---- binary semaphores used as locks ----
 *
 * A semaphore has no owner. LockSight treats a semaphore as a LOCK only when
 * it was created with sem_init(&s, 0, 1): a binary semaphore, the classic
 * "mutex made of a semaphore". The thread that sem_wait()s it is its owner
 * until it sem_post()s it. Semaphores created with any other initial value are
 * used for signaling ("a message has arrived") and are not tracked at all.
 *
 * If a semaphore that looked like a lock is posted by a thread that did not
 * wait on it, it is really being used for signaling. LockSight prints a note
 * and removes it (and all edges through it) from the graph. Threads that
 * thought they held it find out through the generation number (see
 * LockInfo.gen), so nothing in any thread's own list is touched from outside. */

static void print_sem_note(void *sem, void *pc, int tid)
{
    char where[600];
    size_t len = 0;

    load_config();
    if (g_quiet && !g_jsonpath[0])
        return;
    real_lock(&g_print_mutex);
    describe(pc, where, sizeof where, NULL);
    if (g_jsonpath[0]) {
        json_begin("note");
        jprintf(",\"note\":\"semaphore_signaling\",\"thread\":%d,"
                "\"semaphore\":\"%p\",\"posted_at\":", tid, sem);
        jloc(pc);
        jprintf(",\"message\":\"semaphore posted by a thread that did not wait on it; "
                "treated as a signal and no longer tracked\"");
        json_end("");
    }
    if (g_quiet) {
        real_unlock(&g_print_mutex);
        return;
    }
    bprintf(&len, "\n" C_YELLOW "[LockSight] note: semaphore %p was posted by thread T%d, "
                  "which did not wait on it" C_OFF "\n", sem, tid);
    bprintf(&len, C_GREY "      sem_post called at %s\n"
                  "      It is used for signaling, not as a lock, so LockSight stops "
                  "tracking it." C_OFF "\n", where);
    emit(g_out, len);
    real_unlock(&g_print_mutex);
}

int sem_init(sem_t *s, int pshared, unsigned int value)
{
    init_real();
    if (!t_busy) {
        forget_lock(s);
        if (pshared == 0 && value == 1) {
            void *pc = __builtin_return_address(0);
            t_busy = 1;
            real_lock(&g_mutex);
            lookup_or_add_lock(s, pc, KIND_SEM);
            real_unlock(&g_mutex);
            t_busy = 0;
        }
    }
    return real_sem_init(s, pshared, value);
}

int sem_destroy(sem_t *s)
{
    init_real();
    if (!t_busy)
        forget_lock(s);
    return real_sem_destroy(s);
}

int sem_wait(sem_t *s)
{
    init_real();
    if (t_busy)
        return real_sem_wait(s);
    void *pc = __builtin_return_address(0);
    unsigned gen;
    t_busy = 1;
    int id = on_before_lock(s, pc, KIND_SEM, MODE_X, &gen);
    t_busy = 0;
    int rc = real_sem_wait(s);
    if (rc == 0)
        add_hold(s, id, gen, MODE_X, pc);
    return rc;
}

int sem_trywait(sem_t *s)
{
    init_real();
    int rc = real_sem_trywait(s);
    if (rc == 0)
        note_acquired_nowait(s, KIND_SEM, MODE_X, __builtin_return_address(0));
    return rc;
}

int sem_timedwait(sem_t *s, const struct timespec *ts)
{
    init_real();
    int rc = real_sem_timedwait(s, ts);
    if (rc == 0)
        note_acquired_nowait(s, KIND_SEM, MODE_X, __builtin_return_address(0));
    return rc;
}

#if defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 30)
int sem_clockwait(sem_t *s, clockid_t clk, const struct timespec *ts)
{
    init_real();
    int rc = real_sem_clockwait(s, clk, ts);
    if (rc == 0)
        note_acquired_nowait(s, KIND_SEM, MODE_X, __builtin_return_address(0));
    return rc;
}
#endif
#endif

int sem_post(sem_t *s)
{
    init_real();
    if (!t_busy && !remove_hold(s)) {
        /* This thread did not wait on the semaphore. If we track it, it is
         * being used for signaling: drop it from tracking and say so. */
        void *pc = __builtin_return_address(0);
        int dropped = 0;
        t_busy = 1;
        if (t_tid == 0)
            t_tid = __atomic_add_fetch(&g_nthreads, 1, __ATOMIC_RELAXED);
        real_lock(&g_mutex);
        int id = find_lock(s);
        if (id >= 0 && g_locks[id].kind == KIND_SEM) {
            drop_lock(id);
            g_nnotes++;
            dropped = 1;
        }
        real_unlock(&g_mutex);
        if (dropped)
            print_sem_note(s, pc, t_tid);
        t_busy = 0;
    }
    return real_sem_post(s);
}

/* ================================================================== */
/* Part 6: start-up and the summary at exit                             */
/* ================================================================== */

__attribute__((constructor))
static void locksight_start(void)
{
    init_real();
    load_config();
    void *warm[4];
    backtrace(warm, 4);     /* the first backtrace() call loads a helper library;
                               do it now, not in the middle of tracking */
}

__attribute__((destructor))
static void locksight_end(void)
{
    if (g_nlocks_seen == 0 && g_ncycles == 0 && g_nself == 0)
        return;             /* this process never used a mutex: stay quiet */
    print_summary();
}
