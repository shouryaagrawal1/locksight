/*
 * mutex_reuse.c - NOT a bug. LockSight must stay silent.
 *
 * The same two memory addresses hold DIFFERENT mutexes at different times:
 *
 *   phase 1: thread 1 uses  m[0] then m[1]; afterwards both are destroyed.
 *   phase 2: the memory is reused for two NEW mutexes (pthread_mutex_init);
 *            thread 2 uses  m[1] then m[0].
 *   phase 3: the same memory is initialised AGAIN without a destroy (sloppy,
 *            but common); thread 3 uses  m[0] then m[1].
 *
 * Seen by address only, phases 1 and 2 look like an opposite-order bug.
 * They are not: different mutexes. LockSight must forget what it learned
 * about an address when the mutex is destroyed or initialised again.
 */
#include "common.h"

static pthread_mutex_t m[2];

static void *use_pair(void *arg)
{
    int swap = *(int *)arg;
    int x = swap ? 1 : 0;
    int y = swap ? 0 : 1;

    pthread_mutex_lock(&m[x]);
    pthread_mutex_lock(&m[y]);
    pthread_mutex_unlock(&m[y]);
    pthread_mutex_unlock(&m[x]);
    return NULL;
}

static void run_thread(int swap)
{
    pthread_t t;
    pthread_create(&t, NULL, use_pair, &swap);
    pthread_join(t, NULL);
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);

    pthread_mutex_init(&m[0], NULL);                 /* phase 1 */
    pthread_mutex_init(&m[1], NULL);
    run_thread(0);
    pthread_mutex_destroy(&m[0]);
    pthread_mutex_destroy(&m[1]);

    pthread_mutex_init(&m[0], NULL);                 /* phase 2: new mutexes, same memory */
    pthread_mutex_init(&m[1], NULL);
    run_thread(1);

    pthread_mutex_init(&m[0], NULL);                 /* phase 3: re-init without destroy */
    pthread_mutex_init(&m[1], NULL);
    run_thread(0);
    pthread_mutex_destroy(&m[0]);
    pthread_mutex_destroy(&m[1]);

    finished("mutex_reuse");
    return 0;
}
