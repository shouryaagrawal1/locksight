/*
 * rwlock_gate.c - an outer rwlock G around opposite mutex orders.
 *
 *   Thread 1: lock G, then mutex A, then mutex B
 *   Thread 2: lock G, then mutex B, then mutex A
 *
 *   ./rwlock_gate            G is taken as a WRITE lock: only one thread can
 *                            be inside at a time, so the opposite orders can
 *                            never meet. SAFE (a "gate" lock).
 *   ./rwlock_gate --shared   G is taken as a READ lock: both threads are
 *                            inside together, so G protects nothing.
 *                            DEADLOCK possible.
 */
#include "common.h"

static pthread_rwlock_t g = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t  a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t  b = PTHREAD_MUTEX_INITIALIZER;
static int shared;

static void enter(void) { if (shared) pthread_rwlock_rdlock(&g); else pthread_rwlock_wrlock(&g); }

static void *thread1(void *arg)
{
    (void)arg;
    enter();
    pthread_mutex_lock(&a);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_mutex_lock(&b);
    printf("[thread 1] has A and B\n");
    pthread_mutex_unlock(&b);
    pthread_mutex_unlock(&a);
    pthread_rwlock_unlock(&g);
    return NULL;
}

static void *thread2(void *arg)
{
    (void)arg;
    enter();
    pthread_mutex_lock(&b);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_mutex_lock(&a);
    printf("[thread 2] has B and A\n");
    pthread_mutex_unlock(&a);
    pthread_mutex_unlock(&b);
    pthread_rwlock_unlock(&g);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--shared") == 0)
            shared = 1;
    start_watchdog(5);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread1, NULL);
    if (g_lucky) pthread_join(t1, NULL);
    pthread_create(&t2, NULL, thread2, NULL);
    if (!g_lucky) pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    finished("rwlock_gate");
    return 0;
}
