/*
 * rwlock_mixed.c - AB-BA between a MUTEX and a READ-WRITE LOCK.
 *
 *   Thread 1: lock mutex M,        then write-lock R
 *   Thread 2: write-lock R,        then lock mutex M      <-- opposite order
 *
 * LockSight must warn, and name the locks as "mutex" and "rwlock".
 */
#include "common.h"

static pthread_mutex_t  m = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t r = PTHREAD_RWLOCK_INITIALIZER;

static void *thread1(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&m);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_rwlock_wrlock(&r);
    printf("[thread 1] has the mutex and the write lock\n");
    pthread_rwlock_unlock(&r);
    pthread_mutex_unlock(&m);
    return NULL;
}

static void *thread2(void *arg)
{
    (void)arg;
    pthread_rwlock_wrlock(&r);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_mutex_lock(&m);
    printf("[thread 2] has the write lock and the mutex\n");
    pthread_mutex_unlock(&m);
    pthread_rwlock_unlock(&r);
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

    finished("rwlock_mixed");
    return 0;
}
