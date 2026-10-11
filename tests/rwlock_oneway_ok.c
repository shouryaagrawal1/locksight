/*
 * rwlock_oneway_ok.c - a subtle case that looks like AB-BA but is safe.
 *
 *   Thread 1: read-lock A,  then read-lock B
 *   Thread 2: WRITE-lock B, then read-lock A
 *
 * Thread 1 can be blocked by thread 2 (it wants B, thread 2 writes B), but
 * thread 2 is never blocked by thread 1 (two readers of A do not conflict).
 * The wait chain has no loop, so no deadlock is possible.
 */
#include "common.h"

static pthread_rwlock_t a = PTHREAD_RWLOCK_INITIALIZER;
static pthread_rwlock_t b = PTHREAD_RWLOCK_INITIALIZER;

static void *thread1(void *arg)
{
    (void)arg;
    pthread_rwlock_rdlock(&a);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_rwlock_rdlock(&b);
    printf("[thread 1] reads A and B\n");
    pthread_rwlock_unlock(&b);
    pthread_rwlock_unlock(&a);
    return NULL;
}

static void *thread2(void *arg)
{
    (void)arg;
    pthread_rwlock_wrlock(&b);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_rwlock_rdlock(&a);
    printf("[thread 2] writes B, reads A\n");
    pthread_rwlock_unlock(&a);
    pthread_rwlock_unlock(&b);
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

    finished("rwlock_oneway_ok");
    return 0;
}
