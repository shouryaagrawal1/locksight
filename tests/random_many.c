/*
 * random_many.c - a bigger random program: many threads, many locks, mutexes
 * and read-write locks, lots of lock-order loops. It has no known answer; it is
 * used to compare two versions of LockSight with each other (see
 * tests/check.sh): both must give the same number of warnings.
 *
 *   ./random_many SEED
 *
 *   ./random_many SEED PERCENT   (optional) mostly SAFE version: every chain is
 *                                sorted by lock number (a global lock order, so
 *                                no deadlock), except that PERCENT of the
 *                                chains get two locks swapped. PERCENT 0 gives
 *                                a program without any lock-order problem.
 *
 * 8 to 24 threads run one after another. Each takes a chain of 2 to 4 different
 * locks out of 6 to 40 and releases them. About a third of the locks are
 * read-write locks (read or write mode, random).
 */
#include "common.h"

#define MAXL 40

static pthread_mutex_t  mx[MAXL];
static pthread_rwlock_t rw[MAXL];
static int is_rw[MAXL];

typedef struct { int n, lock[4], mode[4]; } Chain;

static void *worker(void *arg)
{
    Chain *c = arg;
    for (int j = 0; j < c->n; j++) {
        int l = c->lock[j];
        if (!is_rw[l])           pthread_mutex_lock(&mx[l]);
        else if (c->mode[j])     pthread_rwlock_rdlock(&rw[l]);
        else                     pthread_rwlock_wrlock(&rw[l]);
    }
    for (int j = c->n - 1; j >= 0; j--) {
        int l = c->lock[j];
        if (!is_rw[l]) pthread_mutex_unlock(&mx[l]);
        else           pthread_rwlock_unlock(&rw[l]);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    srand(argc > 1 ? (unsigned)atoi(argv[1]) : 1);
    int percent = argc > 2 ? atoi(argv[2]) : -1;        /* -1 = fully random chains */
    start_watchdog(20);
    int nlocks = 6 + rand() % 35, nthreads = 8 + rand() % 17;
    if (percent >= 0) nthreads *= 3;                    /* a bigger graph */
    for (int l = 0; l < nlocks; l++) {
        is_rw[l] = rand() % 3 == 0;
        pthread_mutex_init(&mx[l], NULL);
        pthread_rwlock_init(&rw[l], NULL);
    }
    for (int t = 0; t < nthreads; t++) {
        Chain c;
        c.n = 2 + rand() % 3;
        if (c.n > nlocks) c.n = nlocks;
        unsigned long long used = 0;
        for (int j = 0; j < c.n; j++) {
            int l;
            do { l = rand() % nlocks; } while (used & (1ULL << l));
            used |= 1ULL << l;
            c.lock[j] = l;
            c.mode[j] = is_rw[l] ? rand() % 2 : 0;
        }
        if (percent >= 0) {
            for (int i = 1; i < c.n; i++)               /* sort by lock number */
                for (int j = i; j > 0 && c.lock[j - 1] > c.lock[j]; j--) {
                    int tl = c.lock[j]; c.lock[j] = c.lock[j - 1]; c.lock[j - 1] = tl;
                    int tm = c.mode[j]; c.mode[j] = c.mode[j - 1]; c.mode[j - 1] = tm;
                }
            if (rand() % 100 < percent) {               /* break the order once */
                int a = rand() % c.n, b = rand() % c.n;
                int tl = c.lock[a]; c.lock[a] = c.lock[b]; c.lock[b] = tl;
                int tm = c.mode[a]; c.mode[a] = c.mode[b]; c.mode[b] = tm;
            }
        }
        pthread_t th;
        pthread_create(&th, NULL, worker, &c);
        pthread_join(th, NULL);
    }
    return 0;
}
