/*
 * hub_many.c - one "hub" lock held while taking many different locks, then one
 * thread using both orders for each of them.
 *
 *   ./hub_many N        (default 50000)
 *
 * A guard against slowness: when one lock has tens of thousands of edges,
 * every new edge must still be added in constant time. (An earlier design
 * walked all edges of the hub for each new edge: 60000 locks took 38 seconds.)
 * The program is correct (one thread, so no deadlock): LockSight must stay
 * silent, track every lock, and finish quickly.
 */
#include "common.h"

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 50000;
    start_watchdog(60);
    pthread_mutex_t hub = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t *item = malloc(sizeof(pthread_mutex_t) * (size_t)n);
    for (long i = 0; i < n; i++)
        pthread_mutex_init(&item[i], NULL);

    pthread_mutex_lock(&hub);
    for (long i = 0; i < n; i++) {                 /* hub -> item: n edges from one lock */
        pthread_mutex_lock(&item[i]);
        pthread_mutex_unlock(&item[i]);
    }
    pthread_mutex_unlock(&hub);
    for (long i = 0; i < n; i += 50) {             /* item -> hub for some of them */
        pthread_mutex_lock(&item[i]);
        pthread_mutex_lock(&hub);
        pthread_mutex_unlock(&hub);
        pthread_mutex_unlock(&item[i]);
    }
    for (long i = 0; i < n; i++)
        pthread_mutex_destroy(&item[i]);
    free(item);
    finished("hub_many");
    return 0;
}
