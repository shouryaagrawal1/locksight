/*
 * self_deadlock.c - a thread locks a normal mutex it already holds.
 *
 * A normal mutex is not recursive, so the second lock() waits for the thread
 * itself to unlock it, which can never happen. This freezes every time, so
 * there is no "lucky" mode. LockSight must warn BEFORE the freeze.
 */
#include "common.h"

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static void helper(void)
{
    pthread_mutex_lock(&m);          /* the same mutex, a second time */
    printf("[thread 1] never printed\n");
    pthread_mutex_unlock(&m);
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&m);
    printf("[thread 1] got the lock once, now calling helper()\n");
    fflush(stdout);
    helper();                        /* helper() locks it again: deadlock */
    pthread_mutex_unlock(&m);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);

    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_join(t, NULL);

    finished("self_deadlock");
    return 0;
}
