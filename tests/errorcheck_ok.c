/*
 * errorcheck_ok.c - NOT a hang. LockSight must stay silent.
 *
 * An ERROR-CHECKING mutex does not freeze when the same thread locks it twice:
 * the second lock() returns the error EDEADLK straight away, and the program
 * handles it. So there is nothing to warn about.
 */
#include "common.h"
#include <errno.h>

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);

    pthread_mutex_t m;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&m, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_mutex_lock(&m);
    int rc = pthread_mutex_lock(&m);              /* second lock: returns an error */
    printf("[program] second lock returned %d (%s)\n", rc,
           rc == EDEADLK ? "EDEADLK, as expected" : "unexpected");
    pthread_mutex_unlock(&m);
    pthread_mutex_destroy(&m);

    finished("errorcheck_ok");
    return rc == EDEADLK ? 0 : 1;
}
