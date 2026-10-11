/*
 * rwlock_readers_ok.c - opposite orders, but only READ locks: no deadlock.
 *
 *   Thread 1: read-lock A, then read-lock B
 *   Thread 2: read-lock B, then read-lock A
 *
 * Any number of readers may hold a lock together, so nobody ever waits.
 * This is the "shared-reader overlap" that must NOT be reported as AB-BA.
 * (The threads really overlap here and the program finishes normally.)
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
    pthread_rwlock_rdlock(&b);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_rwlock_rdlock(&a);
    printf("[thread 2] reads B and A\n");
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

    finished("rwlock_readers_ok");
    return 0;
}
