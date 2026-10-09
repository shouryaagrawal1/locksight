/*
 * big_ring.c - a deadlock cycle that involves MANY threads.
 *
 * Thread i locks fork i and then fork i+1 (the last thread wraps around to
 * fork 0), like dining_philosophers but with 30 threads by default
 * (./big_ring 100 for 100). The threads run one after another so the program
 * never really freezes, yet the loop of 30 locks is there. LockSight must
 * find it: the number of threads does not matter, only the loop does.
 */
#include "common.h"

#define MAX_N 500

static pthread_mutex_t fork_lock[MAX_N];
static int n = 30;

static void *worker(void *arg)
{
    int id = (int)(size_t)arg;
    pthread_mutex_lock(&fork_lock[id]);
    pthread_mutex_lock(&fork_lock[(id + 1) % n]);
    pthread_mutex_unlock(&fork_lock[(id + 1) % n]);
    pthread_mutex_unlock(&fork_lock[id]);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    for (int i = 1; i < argc; i++)
        if (atoi(argv[i]) >= 2 && atoi(argv[i]) <= MAX_N)
            n = atoi(argv[i]);
    start_watchdog(30);

    for (int i = 0; i < n; i++)
        pthread_mutex_init(&fork_lock[i], NULL);

    for (int i = 0; i < n; i++) {
        pthread_t t;
        pthread_create(&t, NULL, worker, (void *)(size_t)i);
        pthread_join(t, NULL);
    }
    printf("[program] %d threads finished\n", n);
    finished("big_ring");
    return 0;
}
