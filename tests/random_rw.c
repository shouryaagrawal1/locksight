/*
 * random_rw.c - a random program with mutexes AND read-write locks, whose
 * correct answer is worked out by exhaustive search.
 *
 *   ./random_rw SEED
 *
 * From the seed it invents 2 to 4 threads and 3 to 6 locks. Each lock is a
 * mutex or a read-write lock. Each thread takes a short chain of different
 * locks (read or write mode for an rwlock, always exclusive for a mutex) and
 * then releases them all. The threads really run one after another, so the
 * program itself never freezes.
 *
 * BEFORE running, the program decides whether a real deadlock is POSSIBLE:
 * it explores EVERY interleaving of the threads' lock requests (at most a few
 * hundred states) using the real rules of the locks:
 *
 *      a request is blocked if another thread holds that lock in a mode that
 *      conflicts with it (everything conflicts, except read with read);
 *      a deadlock is a reachable state in which some threads are unfinished
 *      and every one of them is blocked.
 *
 * It prints   EXPECT: DEADLOCK   or   EXPECT: SAFE.
 * This oracle knows nothing about lock-order graphs, gates or cycles, so it is
 * an independent check of LockSight's rules for read/write modes.
 * tests/check.sh compares LockSight's verdict with it for many seeds.
 *
 *   ./random_rw SEED --race    really runs the threads TOGETHER with random
 *                              pauses (used to test the oracle itself: a SAFE
 *                              program must never freeze, a DEADLOCK program
 *                              should freeze sometimes; the watchdog is 1 s).
 */
#include "common.h"

#define MAXT 4
#define MAXL 6
#define MAXC 3

static int  nlocks, nthreads;
static int  is_rw[MAXL];
static int  chain[MAXT][MAXC], mode[MAXT][MAXC];   /* mode: 0 = exclusive, 1 = shared */
static int  len[MAXT];
static pthread_mutex_t  mx[MAXL];
static pthread_rwlock_t rw[MAXL];

/* progress[t] = number of locks thread t holds; len[t] means "has all of them
 * and is about to finish"; len[t] + 1 means finished (holds nothing). */
static int  prog[MAXT];
static char seen[5 * 5 * 5 * 5];

static int conflict(int a, int b) { return !(a == 1 && b == 1); }

/* Can thread t make its next move in the current state? */
static int can_move(int t)
{
    if (prog[t] > len[t])
        return 0;                               /* finished: no move */
    if (prog[t] == len[t])
        return 1;                               /* releasing is always possible */
    int l = chain[t][prog[t]], m = mode[t][prog[t]];
    for (int u = 0; u < nthreads; u++) {
        if (u == t)
            continue;
        int n = prog[u] < len[u] ? prog[u] : (prog[u] == len[u] ? len[u] : 0);
        for (int j = 0; j < n; j++)
            if (chain[u][j] == l && conflict(m, mode[u][j]))
                return 0;
    }
    return 1;
}

static int explore(void)       /* returns 1 if a deadlocked state is reachable */
{
    int code = 0;
    for (int t = 0; t < nthreads; t++)
        code = code * 5 + prog[t];
    if (seen[code])
        return 0;
    seen[code] = 1;

    int unfinished = 0, moves = 0;
    for (int t = 0; t < nthreads; t++) {
        if (prog[t] <= len[t]) unfinished++;
        if (can_move(t)) moves++;
    }
    if (unfinished > 0 && moves == 0)
        return 1;                               /* everybody is stuck */

    for (int t = 0; t < nthreads; t++) {
        if (!can_move(t))
            continue;
        prog[t]++;
        int d = explore();
        prog[t]--;
        if (d)
            return 1;
    }
    return 0;
}

static int race;                 /* --race: really run the threads together */

static void *worker(void *arg)
{
    int t = (int)(size_t)arg;
    unsigned rs = (unsigned)t * 7919u + (unsigned)getpid();
    for (int j = 0; j < len[t]; j++) {
        int l = chain[t][j];
        if (race) { rs = rs * 1103515245u + 12345u; usleep((rs >> 16) % 8000); }
        if (!is_rw[l])            pthread_mutex_lock(&mx[l]);
        else if (mode[t][j] == 1) pthread_rwlock_rdlock(&rw[l]);
        else                      pthread_rwlock_wrlock(&rw[l]);
    }
    for (int j = len[t] - 1; j >= 0; j--) {
        int l = chain[t][j];
        if (!is_rw[l]) pthread_mutex_unlock(&mx[l]);
        else           pthread_rwlock_unlock(&rw[l]);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    unsigned seed = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
    srand(seed);
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--race") == 0)
            race = 1;
    start_watchdog(race ? 1 : 10);

    nlocks   = 3 + rand() % 4;
    nthreads = 2 + rand() % 3;
    for (int l = 0; l < nlocks; l++)
        is_rw[l] = rand() % 3 != 0;               /* two thirds are rwlocks */

    for (int t = 0; t < nthreads; t++) {
        int n = 2 + rand() % 2;
        if (n > nlocks)
            n = nlocks;
        len[t] = n;
        unsigned used = 0;
        for (int j = 0; j < n; j++) {
            int l;
            do { l = rand() % nlocks; } while (used & (1u << l));
            used |= 1u << l;
            chain[t][j] = l;
            mode[t][j]  = is_rw[l] ? rand() % 2 : 0;
        }
    }

    printf("EXPECT: %s\n", explore() ? "DEADLOCK" : "SAFE");
    for (int t = 0; t < nthreads; t++) {
        printf("thread %d takes:", t + 1);
        for (int j = 0; j < len[t]; j++)
            printf(" %d%s", chain[t][j], !is_rw[chain[t][j]] ? "m" : mode[t][j] ? "r" : "w");
        printf("\n");
    }
    fflush(stdout);

    for (int l = 0; l < nlocks; l++) {
        pthread_mutex_init(&mx[l], NULL);
        pthread_rwlock_init(&rw[l], NULL);
    }
    if (race) {                  /* all threads at once, with random pauses */
        pthread_t th[MAXT];
        for (int t = 0; t < nthreads; t++)
            pthread_create(&th[t], NULL, worker, (void *)(size_t)t);
        for (int t = 0; t < nthreads; t++)
            pthread_join(th[t], NULL);
        return 0;
    }
    for (int t = 0; t < nthreads; t++) {
        pthread_t th;
        pthread_create(&th, NULL, worker, (void *)(size_t)t);
        pthread_join(th, NULL);
    }
    return 0;
}
