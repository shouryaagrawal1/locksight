/*
 * rwlock_rw_cycle.c - a deadlock made only of READ locks and WRITE locks.
 *
 *   Thread 1: read-lock A,  then write-lock B
 *   Thread 2: read-lock B,  then write-lock A
 *
 * Each thread's read lock blocks the other thread's write request, so both
 * wait forever. LockSight must warn. (Readers alone never block each other;
 * the danger comes from the writer on the second lock.)
 */
#include "common.h"

static pthread_rwlock_t a = PTHREAD_RWLOCK_INITIALIZER;
static pthread_rwlock_t b = PTHREAD_RWLOCK_INITIALIZER;

static void *thread1(void *arg)
{
    (void)arg;
    pthread_rwlock_rdlock(&a);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_rwlock_wrlock(&b);
    printf("[thread 1] read A, write B\n");
    pthread_rwlock_unlock(&b);
    pthread_rwlock_unlock(&a);
    return NULL;
}

static void *thread2(void *arg)
{
    (void)arg;
    pthread_rwlock_rdlock(&b);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_rwlock_wrlock(&a);
    printf("[thread 2] read B, write A\n");
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

    finished("rwlock_rw_cycle");
    return 0;
}
