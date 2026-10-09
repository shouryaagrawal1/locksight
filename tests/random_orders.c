/*
 * random_orders.c - a random program whose correct answer is known.
 *
 *   ./random_orders SEED
 *
 * From the seed it invents a program: 3 to 6 locks and 2 to 4 threads, where
 * each thread takes a short chain of different locks (like "lock 4, then 1,
 * then 3") and releases them again. The threads run one after another, so the
 * program never really freezes.
 *
 * BEFORE running, the program works out by brute force whether a real
 * deadlock is possible, and prints   EXPECT: DEADLOCK   or   EXPECT: SAFE.
 * The brute force does not use LockSight's graph at all: it tries every way
 * of picking one "lock request" from several different threads and checks the
 * textbook condition directly:
 *
 *    - the threads are all different,
 *    - the sets of locks they hold are pairwise disjoint (no common gate),
 *    - each thread asks for a lock that the NEXT thread holds, round the loop.
 *
 * tests/check.sh runs many seeds and compares LockSight's verdict with this
 * expectation. It tests LockSight on programs nobody wrote by hand.
 */
#include "common.h"

#define MAXT 4
#define MAXL 6
#define MAXC 3

static pthread_mutex_t locks[MAXL];
static int nlocks, nthreads;
static int chain[MAXT][MAXC];
static int chainlen[MAXT];

/* One "lock request": thread 'thread' asks for lock 'req' while holding the
 * locks in the bit set 'held'. */
typedef struct { int thread; unsigned held; unsigned req; } Dep;
static Dep deps[MAXT * MAXC];
static int ndeps;
static int pick[MAXT];

/* Extend the chain pick[0..depth-1] by one more request, or close it. */
static int try_chain(int depth)
{
    if (depth >= 2 && (deps[pick[depth - 1]].req & deps[pick[0]].held))
        return 1;                                  /* the loop closes: deadlock possible */
    if (depth == MAXT)
        return 0;

    for (int d = 0; d < ndeps; d++) {
        int ok = 1;
        for (int j = 0; j < depth && ok; j++) {
            if (deps[pick[j]].thread == deps[d].thread) ok = 0;      /* same thread    */
            if (deps[pick[j]].held & deps[d].held)      ok = 0;      /* common gate    */
        }
        if (!ok)
            continue;
        if (depth >= 1 && !(deps[pick[depth - 1]].req & deps[d].held))
            continue;                              /* previous request must hit d's locks */
        pick[depth] = d;
        if (try_chain(depth + 1))
            return 1;
    }
    return 0;
}

static int deadlock_possible(void)
{
    for (int d = 0; d < ndeps; d++) {
        pick[0] = d;
        if (try_chain(1))
            return 1;
    }
    return 0;
}

static void *worker(void *arg)
{
    int t = (int)(size_t)arg;
    for (int j = 0; j < chainlen[t]; j++)
        pthread_mutex_lock(&locks[chain[t][j]]);
    for (int j = chainlen[t] - 1; j >= 0; j--)
        pthread_mutex_unlock(&locks[chain[t][j]]);
    return NULL;
}

int main(int argc, char **argv)
{
    unsigned seed = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
    srand(seed);
    start_watchdog(10);

    nlocks   = 3 + rand() % 4;
    nthreads = 2 + rand() % 3;

    for (int t = 0; t < nthreads; t++) {
        int len = 2 + rand() % 2;
        if (len > nlocks)
            len = nlocks;
        chainlen[t] = len;
        unsigned used = 0;
        for (int j = 0; j < len; j++) {
            int l;
            do { l = rand() % nlocks; } while (used & (1u << l));
            used |= 1u << l;
            chain[t][j] = l;
        }
    }

    /* The lock requests of every thread: asking for chain[t][j] while holding
     * chain[t][0 .. j-1]. */
    for (int t = 0; t < nthreads; t++) {
        unsigned held = 0;
        for (int j = 0; j < chainlen[t]; j++) {
            if (j > 0) {
                deps[ndeps].thread = t;
                deps[ndeps].held   = held;
                deps[ndeps].req    = 1u << chain[t][j];
                ndeps++;
            }
            held |= 1u << chain[t][j];
        }
    }

    printf("EXPECT: %s\n", deadlock_possible() ? "DEADLOCK" : "SAFE");
    for (int t = 0; t < nthreads; t++) {
        printf("thread %d takes:", t + 1);
        for (int j = 0; j < chainlen[t]; j++)
            printf(" %d", chain[t][j]);
        printf("\n");
    }
    fflush(stdout);

    for (int i = 0; i < nlocks; i++)
        pthread_mutex_init(&locks[i], NULL);

    for (int t = 0; t < nthreads; t++) {
        pthread_t th;
        pthread_create(&th, NULL, worker, (void *)(size_t)t);
        pthread_join(th, NULL);
    }
    return 0;
}
