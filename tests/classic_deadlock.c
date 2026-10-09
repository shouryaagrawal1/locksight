/*
 * classic_deadlock.c - the textbook deadlock.
 *
 *   Thread 1: lock A, then lock B
 *   Thread 2: lock B, then lock A      <-- opposite order = danger
 */
#include "common.h"

static pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;

static void *thread1(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock_a);
    sleep_ms(g_lucky ? 0 : 200);       /* gives thread 2 time to grab B */
    pthread_mutex_lock(&lock_b);
    printf("[thread 1] has both locks\n");
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
    return NULL;
}

static void *thread2(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock_b);
    sleep_ms(g_lucky ? 0 : 200);       /* gives thread 1 time to grab A */
    pthread_mutex_lock(&lock_a);
    printf("[thread 2] has both locks\n");
    pthread_mutex_unlock(&lock_a);
    pthread_mutex_unlock(&lock_b);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread1, NULL);
    if (g_lucky) pthread_join(t1, NULL);     /* lucky: finish thread 1 first */
    pthread_create(&t2, NULL, thread2, NULL);
    if (!g_lucky) pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    finished("classic_deadlock");
    return 0;
}
