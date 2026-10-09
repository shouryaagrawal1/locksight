/*
 * worker_pool_outlier.c - one odd thread among many.
 *
 * Twenty worker threads all take A then B (consistent, fine). Then ONE more
 * thread takes B then A. That single outlier makes a real deadlock possible
 * with any of the 20 workers. LockSight must warn, even though it only keeps a
 * few threads in memory for each lock pair.
 */
#include "common.h"

#define WORKERS 20

static pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock_a);
    pthread_mutex_lock(&lock_b);
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
    return NULL;
}

static void *outlier(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock_b);              /* the odd one: B then A */
    pthread_mutex_lock(&lock_a);
    pthread_mutex_unlock(&lock_a);
    pthread_mutex_unlock(&lock_b);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(10);

    pthread_t t;
    for (int i = 0; i < WORKERS; i++) {
        pthread_create(&t, NULL, worker, NULL);
        pthread_join(t, NULL);
    }
    pthread_create(&t, NULL, outlier, NULL);
    pthread_join(t, NULL);

    finished("worker_pool_outlier");
    return 0;
}
