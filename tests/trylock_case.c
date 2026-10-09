/*
 * trylock_case.c - NOT a bug. LockSight must stay silent.
 *
 *   Thread 1: lock A, then lock B
 *   Thread 2: lock B, then TRYLOCK A       <-- opposite order, but trylock
 *                                              never waits: if A is busy,
 *                                              thread 2 gives up its B and
 *                                              backs off.
 *
 * Because trylock cannot block, it cannot be part of a deadlock, so
 * LockSight adds no edge for it.
 */
#include "common.h"

static pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;

static void *thread1(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock_a);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_mutex_lock(&lock_b);
    printf("[thread 1] has A and B\n");
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
    return NULL;
}

static void *thread2(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock_b);
    sleep_ms(g_lucky ? 0 : 100);
    if (pthread_mutex_trylock(&lock_a) == 0) {
        printf("[thread 2] has B and A\n");
        pthread_mutex_unlock(&lock_a);
    } else {
        printf("[thread 2] A is busy, backing off\n");
    }
    pthread_mutex_unlock(&lock_b);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread1, NULL);
    if (g_lucky) pthread_join(t1, NULL);
    pthread_create(&t2, NULL, thread2, NULL);
    if (!g_lucky) pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    finished("trylock_case");
    return 0;
}
