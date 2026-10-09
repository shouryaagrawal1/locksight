/*
 * recursive_ok.c - NOT a bug. LockSight must stay silent.
 *
 * A RECURSIVE mutex may be locked again by the thread that already holds it
 * (it just counts how many times). Two threads do this while also taking a
 * second lock, always in the same order.
 */
#include "common.h"

static pthread_mutex_t rec;                       /* recursive, set up in main */
static pthread_mutex_t other = PTHREAD_MUTEX_INITIALIZER;
static int counter = 0;

static void inner(void)
{
    pthread_mutex_lock(&rec);                     /* locked a 2nd and 3rd time */
    pthread_mutex_lock(&rec);
    pthread_mutex_lock(&other);
    counter++;
    pthread_mutex_unlock(&other);
    pthread_mutex_unlock(&rec);
    pthread_mutex_unlock(&rec);
}

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200; i++) {
        pthread_mutex_lock(&rec);
        inner();
        pthread_mutex_unlock(&rec);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&rec, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, worker, NULL);
    pthread_create(&t2, NULL, worker, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("[program] counter = %d (expected 400)\n", counter);
    finished("recursive_ok");
    return 0;
}
