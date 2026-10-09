/*
 * single_thread_both_orders.c - NOT a bug. LockSight must stay silent.
 *
 * One thread takes A then B in one part of its job, and B then A in another.
 * The two orders look opposite, but a thread can never wait for itself,
 * so no deadlock is possible. (Filter 1: a dangerous cycle needs
 * at least two different threads.)
 */
#include "common.h"

static pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&lock_a);            /* first part of the job: A then B */
    pthread_mutex_lock(&lock_b);
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);

    pthread_mutex_lock(&lock_b);            /* second part: B then A */
    pthread_mutex_lock(&lock_a);
    pthread_mutex_unlock(&lock_a);
    pthread_mutex_unlock(&lock_b);

    printf("[thread 1] used both orders, all fine\n");
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);

    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_join(t, NULL);

    finished("single_thread_both_orders");
    return 0;
}
