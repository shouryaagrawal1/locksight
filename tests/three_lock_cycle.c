/*
 * three_lock_cycle.c - a deadlock cycle across THREE threads.
 *
 *   Thread 1: lock A, then lock B
 *   Thread 2: lock B, then lock C
 *   Thread 3: lock C, then lock A      <-- closes the loop A -> B -> C -> A
 *
 * No two threads use opposite orders directly, so this is harder to spot by eye.
 */
#include "common.h"

static pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_c = PTHREAD_MUTEX_INITIALIZER;

static void take_two(pthread_mutex_t *first, pthread_mutex_t *second, int id)
{
    pthread_mutex_lock(first);
    sleep_ms(g_lucky ? 0 : 200);
    pthread_mutex_lock(second);
    printf("[thread %d] has both locks\n", id);
    pthread_mutex_unlock(second);
    pthread_mutex_unlock(first);
}

static void *thread1(void *arg) { (void)arg; take_two(&lock_a, &lock_b, 1); return NULL; }
static void *thread2(void *arg) { (void)arg; take_two(&lock_b, &lock_c, 2); return NULL; }
static void *thread3(void *arg) { (void)arg; take_two(&lock_c, &lock_a, 3); return NULL; }

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);

    void *(*funcs[3])(void *) = { thread1, thread2, thread3 };
    pthread_t t[3];

    for (int i = 0; i < 3; i++) {
        pthread_create(&t[i], NULL, funcs[i], NULL);
        if (g_lucky) pthread_join(t[i], NULL);   /* lucky: one at a time */
    }
    if (!g_lucky)
        for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);

    finished("three_lock_cycle");
    return 0;
}
