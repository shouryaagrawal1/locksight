/*
 * bench_locks.c - measures what LockSight costs.
 *
 *   ./bench_locks SCENARIO ITERATIONS
 *
 * Prints one line:  scenario  iterations  total_ms  ns_per_iteration
 * One "iteration" = one lock/unlock sequence of the scenario (nested scenarios
 * take 2 locks per iteration).
 *
 * Scenarios:
 *   nop          does nothing (measures program start-up)
 *   single       1 thread, 1 lock
 *   nested       1 thread, 2 locks taken in the same order every time
 *   contended    4 threads fight over 1 lock
 *   nested4      4 threads, 2 shared locks, always taken in the same order
 *   private4     4 threads, each with its own pair of locks (no contention)
 *   chain500     1 thread walks through 500 locks while holding the previous one
 *   realistic    4 threads, 8 shared locks, some "work" between locks
 *   rwread4      4 threads read-lock one rwlock
 *   rwnested4    4 threads: mutex, then write-lock an rwlock (same order always)
 *   sem4         4 threads share one binary semaphore (wait / post)
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NT 4
static long g_iters;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static volatile long sink;
static void work(int n) { for (int i = 0; i < n; i++) sink += i; }

static pthread_mutex_t A = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t B = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t priv[NT][2];
static pthread_mutex_t chain[500];
static pthread_mutex_t pool[8];
static pthread_rwlock_t RW = PTHREAD_RWLOCK_INITIALIZER;
static sem_t SEM;
static long counter;
static volatile long rdsink;

static void *t_contended(void *a)
{
    (void)a;
    for (long i = 0; i < g_iters / NT; i++) {
        pthread_mutex_lock(&A); counter++; pthread_mutex_unlock(&A);
    }
    return NULL;
}
static void *t_nested4(void *a)
{
    (void)a;
    for (long i = 0; i < g_iters / NT; i++) {
        pthread_mutex_lock(&A); pthread_mutex_lock(&B);
        counter++;
        pthread_mutex_unlock(&B); pthread_mutex_unlock(&A);
    }
    return NULL;
}
static void *t_rwread(void *a)
{
    (void)a;
    for (long i = 0; i < g_iters / NT; i++) {
        pthread_rwlock_rdlock(&RW); rdsink += counter; pthread_rwlock_unlock(&RW);
    }
    return NULL;
}
static void *t_rwnested(void *a)
{
    (void)a;
    for (long i = 0; i < g_iters / NT; i++) {
        pthread_mutex_lock(&A); pthread_rwlock_wrlock(&RW);
        counter++;
        pthread_rwlock_unlock(&RW); pthread_mutex_unlock(&A);
    }
    return NULL;
}
static void *t_sem(void *a)
{
    (void)a;
    for (long i = 0; i < g_iters / NT; i++) {
        sem_wait(&SEM); counter++; sem_post(&SEM);
    }
    return NULL;
}
static void *t_private(void *a)
{
    int id = (int)(long)a;
    for (long i = 0; i < g_iters / NT; i++) {
        pthread_mutex_lock(&priv[id][0]); pthread_mutex_lock(&priv[id][1]);
        counter++;
        pthread_mutex_unlock(&priv[id][1]); pthread_mutex_unlock(&priv[id][0]);
    }
    return NULL;
}
static void *t_realistic(void *a)
{
    unsigned s = (unsigned)(long)a * 7919u + 1;
    for (long i = 0; i < g_iters / NT; i++) {
        s = s * 1103515245u + 12345u;
        int x = (s >> 8) % 8, y = (s >> 16) % 8;
        if (x == y) y = (y + 1) % 8;
        int lo = x < y ? x : y, hi = x < y ? y : x;   /* ordered: no deadlock */
        pthread_mutex_lock(&pool[lo]);
        work(50);
        pthread_mutex_lock(&pool[hi]);
        work(50);
        pthread_mutex_unlock(&pool[hi]);
        pthread_mutex_unlock(&pool[lo]);
    }
    return NULL;
}

static void run_threads(void *(*fn)(void *))
{
    pthread_t t[NT];
    for (long i = 0; i < NT; i++) pthread_create(&t[i], NULL, fn, (void *)i);
    for (int i = 0; i < NT; i++) pthread_join(t[i], NULL);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s SCENARIO ITERATIONS\n", argv[0]); return 2; }
    const char *sc = argv[1];
    g_iters = atol(argv[2]);
    for (int i = 0; i < NT; i++) { pthread_mutex_init(&priv[i][0], NULL); pthread_mutex_init(&priv[i][1], NULL); }
    for (int i = 0; i < 500; i++) pthread_mutex_init(&chain[i], NULL);
    for (int i = 0; i < 8; i++) pthread_mutex_init(&pool[i], NULL);
    sem_init(&SEM, 0, 1);

    double t0 = now_ms();
    if (!strcmp(sc, "nop")) {
        g_iters = 1;
    } else if (!strcmp(sc, "single")) {
        for (long i = 0; i < g_iters; i++) { pthread_mutex_lock(&A); counter++; pthread_mutex_unlock(&A); }
    } else if (!strcmp(sc, "nested")) {
        for (long i = 0; i < g_iters; i++) {
            pthread_mutex_lock(&A); pthread_mutex_lock(&B); counter++;
            pthread_mutex_unlock(&B); pthread_mutex_unlock(&A);
        }
    } else if (!strcmp(sc, "contended")) run_threads(t_contended);
    else if (!strcmp(sc, "nested4"))     run_threads(t_nested4);
    else if (!strcmp(sc, "private4"))    run_threads(t_private);
    else if (!strcmp(sc, "chain500")) {
        for (long i = 0; i < g_iters; i++) {
            pthread_mutex_lock(&chain[0]);
            for (int k = 1; k < 500; k++) { pthread_mutex_lock(&chain[k]); pthread_mutex_unlock(&chain[k - 1]); }
            pthread_mutex_unlock(&chain[499]);
        }
        g_iters *= 500;
    } else if (!strcmp(sc, "realistic")) run_threads(t_realistic);
    else if (!strcmp(sc, "rwread4"))   run_threads(t_rwread);
    else if (!strcmp(sc, "rwnested4")) run_threads(t_rwnested);
    else if (!strcmp(sc, "sem4"))      run_threads(t_sem);
    else { fprintf(stderr, "unknown scenario %s\n", sc); return 2; }
    double ms = now_ms() - t0;
    printf("%s %ld %.2f %.1f\n", sc, g_iters, ms, ms * 1e6 / (double)g_iters);
    return 0;
}
