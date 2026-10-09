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
 * OTHER THINGS HANDLED
 * --------------------
 *  - Self-deadlock: a thread locks a normal mutex it already holds.
 *  - Recursive / error-checking mutexes may be locked again: no warning.
 *  - pthread_mutex_destroy / pthread_mutex_init: the history of a mutex is
 *    forgotten, because the same memory address may later hold a new mutex.
 *  - pthread_mutex_trylock never waits, so it adds no edge.
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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Limits. Fixed-size tables mean we never call malloc while tracking, */
/* which keeps the interceptor simple and safe.                        */
/* ------------------------------------------------------------------ */
#define MAX_LOCKS      2048     /* mutexes tracked at the same time          */
#define HASH_SIZE      8192     /* lock-address hash table (> 4 x MAX_LOCKS) */
#define MAX_EDGES      16384    /* "A held while taking B" records           */
#define MAX_HELD       64       /* locks one thread can hold at once         */
#define MAX_FRAMES     8        /* call-stack frames saved per edge          */
#define MAX_CYCLE      16       /* longest cycle printed in full             */
#define MAX_DEPTH      256      /* longest cycle we search for               */
#define MAX_PER_EDGE   4        /* different threads remembered per A->B pair */
#define GATE_BITS      256      /* locks 0..255 can act as "gate" locks      */
#define GATE_WORDS     (GATE_BITS / 64)
#define SEARCH_BUDGET  200000   /* steps one cycle search may take           */
#define MAX_WARNINGS   20       /* stop printing after this many warnings    */
#define OUT_SIZE       24576    /* size of the warning text buffer           */

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

/* One tracked mutex. We identify a mutex by its memory address. */
typedef struct {
    void *addr;        /* address of the pthread_mutex_t                    */
    void *first_pc;    /* where in the program we first saw it being used   */
} LockInfo;

/* One record of the lock-order graph:  "thread 'tid' asked for lock 'to'
 * while holding lock 'from'".  The same pair from -> to can have a few
 * records, one per thread (up to MAX_PER_EDGE). */
typedef struct {
    int      from, to;               /* lock ids                              */
    int      tid;                    /* the thread this record belongs to     */
    int      next_out, next_in;      /* lists of edges leaving 'from' and
                                        entering 'to' (index+1, 0 = end)      */
    uint64_t held[GATE_WORDS];       /* set of locks the thread held at that
                                        moment (bit n = lock id n)            */
    void    *from_pc;                /* where 'from' had been acquired        */
    void    *to_stack[MAX_FRAMES];   /* call stack where 'to' was requested   */
    int      to_depth;               /* number of valid entries in to_stack   */
} Edge;

/* One lock currently held by the calling thread. */
typedef struct {
    void *addr;
    int   id;
    void *pc;          /* where it was acquired */
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

/* Scratch space for the cycle search. */
static unsigned g_mark[MAX_LOCKS];          /* "on the current path" marks     */
static unsigned g_gen;
static int      g_chosen[MAX_DEPTH];        /* edge indices of the current path */
static int      g_depth;
static long     g_budget;

static int      g_nthreads, g_ncycles, g_nself;
static int      g_dropped_locks, g_dropped_edges, g_budget_hits;
static int      g_warnings_printed;

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

static void init_real(void)
{
    if (real_lock && real_trylock && real_unlock && real_init && real_destroy)
        return;
    /* Looking these up twice (two threads racing) is harmless. */
    real_lock    = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_trylock = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_unlock  = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    real_init    = (int (*)(pthread_mutex_t *, const pthread_mutexattr_t *))
                       dlsym(RTLD_NEXT, "pthread_mutex_init");
    real_destroy = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_destroy");
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
 *   LOCKSIGHT_STATUS=file the launcher reads this to learn whether anything
 *                         was reported, so it can return exit status 3
 *   LOCKSIGHT_COLOR=always|never|auto   (auto = only when stderr is a terminal)
 *   NO_COLOR=1            turns colors off (a common convention)
 */
static int  g_config_loaded;
static int  g_quiet, g_strict, g_color;
static char g_logpath[1024];
static char g_statuspath[1024];

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
        bprintf(&len, C_GREY "    lock#%-3d %p  first used at %s" C_OFF "\n",
                c->e[i].from + 1, c->l[i].addr, where);
    }
    bprintf(&len, "\n");

    /* 3. One block per edge: which thread took which lock while holding which. */
    for (int i = 0; i < c->n; i++) {
        const Edge *e = &c->e[i];
        bprintf(&len, C_BOLD "  Thread T%d took lock#%d while holding lock#%d" C_OFF "\n",
                e->tid, e->to + 1, e->from + 1);

        describe(e->from_pc, where, sizeof where, NULL);
        bprintf(&len, C_GREY "      lock#%d was acquired at   %s" C_OFF "\n", e->from + 1, where);

        describe(e->to_depth > 0 ? e->to_stack[0] : NULL, where, sizeof where, NULL);
        bprintf(&len, C_GREY "      lock#%d was requested at  %s" C_OFF "\n", e->to + 1, where);

        bprint_callers(&len, e->to_stack, e->to_depth);
        bprintf(&len, "\n");
    }

    bprintf(&len, "  " C_GREEN "Fix:" C_OFF " make every thread take these locks in the same order.\n");
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

    bprintf(&len, "\n  " C_GREEN "Fix:" C_OFF " unlock it before locking again, or create the mutex with\n"
                  "  PTHREAD_MUTEX_RECURSIVE if locking it twice is intended.\n");
    write_out(len);
    real_unlock(&g_print_mutex);
}

/* The summary block (not printed with --quiet). */
static void print_summary(void)
{
    load_config();
    if (g_quiet)
        return;

    size_t len = 0;
    real_lock(&g_print_mutex);

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
static int lookup_or_add_lock(void *addr, void *pc)
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

static void unlink_out(int from, int ei)
{
    int *p = &g_first_out[from];
    while (*p != 0) {
        if (*p - 1 == ei) {
            *p = g_edges[ei].next_out;
            return;
        }
        p = &g_edges[*p - 1].next_out;
    }
}

static void unlink_in(int to, int ei)
{
    int *p = &g_first_in[to];
    while (*p != 0) {
        if (*p - 1 == ei) {
            *p = g_edges[ei].next_in;
            return;
        }
        p = &g_edges[*p - 1].next_in;
    }
}

/* Forget a lock completely: all edges leaving or entering it, its address,
 * and its id (which can then be given to a new lock). */
static void drop_lock(int id)
{
    for (int e = g_first_out[id]; e != 0; ) {
        int ei = e - 1, nxt = g_edges[ei].next_out;
        unlink_in(g_edges[ei].to, ei);
        free_edge(ei);
        e = nxt;
    }
    g_first_out[id] = 0;

    for (int e = g_first_in[id]; e != 0; ) {
        int ei = e - 1, nxt = g_edges[ei].next_in;
        unlink_out(g_edges[ei].from, ei);
        free_edge(ei);
        e = nxt;
    }
    g_first_in[id] = 0;

    hash_remove(g_locks[id].addr);
    g_locks[id].addr = NULL;
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
static int note_dependency(int from, int to, const uint64_t *held,
                           void *from_pc, void *to_pc)
{
    int same_pair = 0;
    for (int ei = g_first_out[from]; ei != 0; ei = g_edges[ei - 1].next_out) {
        Edge *e = &g_edges[ei - 1];
        if (e->to != to)
            continue;
        same_pair++;
        if (e->tid == t_tid) {
            int changed = 0;
            for (int w = 0; w < GATE_WORDS; w++) {
                uint64_t now = e->held[w] & held[w];
                if (now != e->held[w]) {
                    e->held[w] = now;
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
    memcpy(e->held, held, sizeof e->held);
    e->from_pc  = from_pc;
    e->next_out = g_first_out[from];
    g_first_out[from] = ei + 1;
    e->next_in  = g_first_in[to];
    g_first_in[to] = ei + 1;
    e->to_depth = capture_stack(to_pc, e->to_stack, MAX_FRAMES);
    g_nedges_seen++;
    return ei;
}

/* ---- Finding a dangerous cycle ---- */

/* Can records a and b be two links of the same real deadlock? They must come
 * from different threads, and the threads must hold no lock in common. */
static int compatible(const Edge *a, const Edge *b)
{
    if (a->tid == b->tid)
        return 0;
    for (int w = 0; w < GATE_WORDS; w++)
        if (a->held[w] & b->held[w])
            return 0;                   /* a common "gate" lock protects both */
    return 1;
}

/* Depth-first search: is there a chain of records from 'cur' back to 'target'
 * in which every record is compatible with all the others? The records of the
 * chain so far are in g_chosen[0 .. g_depth-1]. */
static int search(int cur, int target)
{
    if (--g_budget < 0 || g_depth >= MAX_DEPTH)
        return 0;

    for (int ei = g_first_out[cur]; ei != 0; ei = g_edges[ei - 1].next_out) {
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

        if (e->to == target) {                   /* the loop is closed */
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
    const Edge *first = &g_edges[e0];
    g_gen++;
    if (g_gen == 0)
        g_gen = 1;
    g_mark[first->to] = g_gen;
    g_chosen[0] = e0;
    g_depth     = 1;
    g_budget    = SEARCH_BUDGET;

    int found = search(first->to, first->from);
    if (!found && g_budget < 0)
        g_budget_hits++;
    g_mark[first->to] = 0;
    return found;
}

/* ================================================================== */
/* Part 4: what happens just before a lock is taken                     */
/* ================================================================== */

/* Returns the lock id (or -1). If a new problem appears, it is printed. */
static int on_before_lock(void *addr, void *pc)
{
    Cycle        cyc;
    SelfDeadlock sd;
    int          have_self = 0;
    cyc.n = 0;

    if (t_tid == 0)
        t_tid = __atomic_add_fetch(&g_nthreads, 1, __ATOMIC_RELAXED);

    /* The set of locks this thread holds right now (used to find gate locks). */
    uint64_t held[GATE_WORDS];
    memset(held, 0, sizeof held);
    for (int i = 0; i < t_nheld; i++) {
        int hid = t_held[i].id;
        if (hid >= 0 && hid < GATE_BITS)
            held[hid >> 6] |= 1ULL << (hid & 63);
    }

    real_lock(&g_mutex);

    int id = lookup_or_add_lock(addr, pc);
    if (id >= 0) {
        for (int i = 0; i < t_nheld; i++) {
            int h = t_held[i].id;
            if (h < 0)
                continue;

            if (h == id) {
                /* The thread already holds this very lock. For a normal mutex
                 * that means it will wait for itself forever. Recursive and
                 * error-checking mutexes are allowed to be locked again. */
                int kind = ((pthread_mutex_t *)addr)->__data.__kind & 3;
                if (!have_self &&
                    (kind == PTHREAD_MUTEX_NORMAL || kind == PTHREAD_MUTEX_ADAPTIVE_NP)) {
                    have_self       = 1;
                    sd.lock         = g_locks[id];
                    sd.held_pc      = t_held[i].pc;
                    sd.req_depth    = capture_stack(pc, sd.req_stack, MAX_FRAMES);
                    sd.id           = id;
                    sd.tid          = t_tid;
                    sd.number       = ++g_nself;
                }
                continue;
            }

            int e0 = note_dependency(h, id, held, t_held[i].pc, pc);
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

int pthread_mutex_lock(pthread_mutex_t *m)
{
    init_real();
    if (t_busy)
        return real_lock(m);

    void *pc = __builtin_return_address(0);
    t_busy = 1;
    int id = on_before_lock(m, pc);     /* record edges + look for problems FIRST */
    t_busy = 0;

    int rc = real_lock(m);              /* ...then really take the lock           */
    if (rc == 0 && t_nheld < MAX_HELD) {
        t_held[t_nheld].addr = m;
        t_held[t_nheld].id   = id;
        t_held[t_nheld].pc   = pc;
        t_nheld++;
    }
    return rc;
}

/* trylock never waits, so it cannot be part of a deadlock by itself: we add
 * no edge for it. But if it succeeds the thread now HOLDS the lock, and locks
 * taken later while holding it must still be recorded. */
int pthread_mutex_trylock(pthread_mutex_t *m)
{
    init_real();
    int rc = real_trylock(m);
    if (rc == 0 && !t_busy && t_nheld < MAX_HELD) {
        void *pc = __builtin_return_address(0);
        t_busy = 1;
        if (t_tid == 0)
            t_tid = __atomic_add_fetch(&g_nthreads, 1, __ATOMIC_RELAXED);
        real_lock(&g_mutex);
        int id = lookup_or_add_lock(m, pc);
        real_unlock(&g_mutex);
        t_busy = 0;

        t_held[t_nheld].addr = m;
        t_held[t_nheld].id   = id;
        t_held[t_nheld].pc   = pc;
        t_nheld++;
    }
    return rc;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    init_real();
    /* Forget the lock (search from the newest; locks may be released in any order). */
    for (int i = t_nheld - 1; i >= 0; i--) {
        if (t_held[i].addr == m) {
            for (int j = i; j < t_nheld - 1; j++)
                t_held[j] = t_held[j + 1];
            t_nheld--;
            break;
        }
    }
    return real_unlock(m);
}

/* The same memory address can hold a different mutex later (a mutex is
 * destroyed and another is created in the same place, or freed memory is
 * reused). Whatever we learned about the old one must not leak into the new
 * one, so on init and destroy we forget the address completely. */
static void forget_mutex(void *addr)
{
    for (int i = t_nheld - 1; i >= 0; i--) {     /* also in this thread's own list */
        if (t_held[i].addr == addr) {
            for (int j = i; j < t_nheld - 1; j++)
                t_held[j] = t_held[j + 1];
            t_nheld--;
        }
    }
    t_busy = 1;
    real_lock(&g_mutex);
    int id = find_lock(addr);
    if (id >= 0)
        drop_lock(id);
    real_unlock(&g_mutex);
    t_busy = 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    init_real();
    if (!t_busy)
        forget_mutex(m);
    return real_init(m, attr);
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    init_real();
    if (!t_busy)
        forget_mutex(m);
    return real_destroy(m);
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
