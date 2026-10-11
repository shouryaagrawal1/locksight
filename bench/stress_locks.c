/*
 * stress_locks.c - programs with MANY locks, to test LockSight's tables.
 *
 *   ./stress_locks SCENARIO N
 *
 * Prints:  scenario N seconds
 *
 *   hub_fwd      one hub lock held while taking N different locks, one by one
 *   hub_both     the same, then each of the N locks followed by the hub lock
 *                (one thread, both orders: legal, but LockSight must search)
 *   hub_destroy  hub_fwd, then all N locks destroyed (hub keeps N edges until then)
 *   pool4        4 threads, a pool of N locks: each step locks a random pool lock,
 *                sometimes a second one (always the higher index first = safe)
 *   churn        N times: create a lock, lock it, unlock it, destroy it
 *   hub_threads  4 threads, hub lock then a random pool lock (N locks): many edges
 *                from one lock, taken by several threads
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static pthread_mutex_t hub = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *item;
static long n;

static void *pool_worker(void *a)
{
    unsigned s = (unsigned)(long)a * 2654435761u + 1;
    for (long i = 0; i < n * 2; i++) {
        s = s * 1103515245u + 12345u;
        long x = (s >> 8) % n, y = (s >> 3) % n;
        if (x == y) { pthread_mutex_lock(&item[x]); pthread_mutex_unlock(&item[x]); continue; }
        long hi = x > y ? x : y, lo = x > y ? y : x;
        pthread_mutex_lock(&item[hi]); pthread_mutex_lock(&item[lo]);
        pthread_mutex_unlock(&item[lo]); pthread_mutex_unlock(&item[hi]);
    }
    return NULL;
}
static void *hub_worker(void *a)
{
    unsigned s = (unsigned)(long)a * 2654435761u + 7;
    for (long i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        long x = (s >> 8) % n;
        pthread_mutex_lock(&hub); pthread_mutex_lock(&item[x]);
        pthread_mutex_unlock(&item[x]); pthread_mutex_unlock(&hub);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s SCENARIO N\n", argv[0]); return 2; }
    const char *sc = argv[1];
    n = atol(argv[2]);
    item = malloc(sizeof(pthread_mutex_t) * (size_t)n);
    for (long i = 0; i < n; i++) pthread_mutex_init(&item[i], NULL);

    double t0 = now();
    if (!strcmp(sc, "hub_fwd") || !strcmp(sc, "hub_both") || !strcmp(sc, "hub_destroy")) {
        pthread_mutex_lock(&hub);
        for (long i = 0; i < n; i++) { pthread_mutex_lock(&item[i]); pthread_mutex_unlock(&item[i]); }
        pthread_mutex_unlock(&hub);
        if (!strcmp(sc, "hub_both")) {
            for (long i = 0; i < n; i++) {
                pthread_mutex_lock(&item[i]); pthread_mutex_lock(&hub);
                pthread_mutex_unlock(&hub); pthread_mutex_unlock(&item[i]);
            }
        }
        if (!strcmp(sc, "hub_destroy"))
            for (long i = 0; i < n; i++) pthread_mutex_destroy(&item[i]);
    } else if (!strcmp(sc, "pool4") || !strcmp(sc, "hub_threads")) {
        pthread_t t[4];
        for (long i = 0; i < 4; i++) pthread_create(&t[i], NULL, !strcmp(sc, "pool4") ? pool_worker : hub_worker, (void *)i);
        for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    } else if (!strcmp(sc, "churn")) {
        pthread_mutex_t m;
        for (long i = 0; i < n; i++) {
            pthread_mutex_init(&m, NULL); pthread_mutex_lock(&hub); pthread_mutex_lock(&m);
            pthread_mutex_unlock(&m); pthread_mutex_unlock(&hub); pthread_mutex_destroy(&m);
        }
    } else { fprintf(stderr, "unknown scenario\n"); return 2; }
    printf("%s %ld %.2f\n", sc, n, now() - t0);
    return 0;
}
