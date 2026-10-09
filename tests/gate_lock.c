/*
 * gate_lock.c - NOT a bug. LockSight must stay silent.
 *
 *   Thread 1: lock GATE, then A, then B
 *   Thread 2: lock GATE, then B, then A      <-- opposite order of A and B ...
 *
 * ... but both threads take the GATE lock first, so only one of them can be
 * inside at a time. They can never block each other. (Filter 2: a dangerous
 * cycle must not be protected by a lock that all the threads share.)
 */
#include "common.h"

static pthread_mutex_t gate   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;

static void *thread1(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&lock_a);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_mutex_lock(&lock_b);
    printf("[thread 1] has gate, A and B\n");
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
    pthread_mutex_unlock(&gate);
    return NULL;
}

static void *thread2(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&lock_b);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_mutex_lock(&lock_a);
    printf("[thread 2] has gate, B and A\n");
    pthread_mutex_unlock(&lock_a);
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&gate);
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

    finished("gate_lock");
    return 0;
}
