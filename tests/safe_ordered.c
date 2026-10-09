/*
 * safe_ordered.c - a CORRECT program. LockSight must stay silent here.
 *
 *   Thread 1: lock A, then lock B
 *   Thread 2: lock A, then lock B      <-- same order everywhere = no deadlock
 */
#include "common.h"

static pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;
static int counter = 0;

static void *worker(void *arg)
{
    int id = *(int *)arg;
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&lock_a);
        pthread_mutex_lock(&lock_b);
        counter++;
        pthread_mutex_unlock(&lock_b);
        pthread_mutex_unlock(&lock_a);
    }
    printf("[thread %d] done\n", id);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);

    pthread_t t1, t2;
    int id1 = 1, id2 = 2;
    pthread_create(&t1, NULL, worker, &id1);
    pthread_create(&t2, NULL, worker, &id2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("[program] counter = %d (expected 2000)\n", counter);
    finished("safe_ordered");
    return 0;
}
