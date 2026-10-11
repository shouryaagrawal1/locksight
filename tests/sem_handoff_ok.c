/*
 * sem_handoff_ok.c - a semaphore that looks like a lock but is used for
 * signaling, and must not confuse LockSight.
 *
 *   s = sem_init(.., 1)         (looks like a lock)
 *   T1: sem_wait(s)             "takes" it and keeps it
 *   T2: sem_post(s)             posts it: T2 never waited, so s is a signal
 *
 * LockSight prints a NOTE and stops tracking s (the memory of s is then
 * reused for another lock). Afterwards T2 uses two new mutexes in both orders
 * (P then Q, later Q then P: fine for one thread), and T1 takes Q. If LockSight
 * still believed that T1 holds "the old s", it would invent a cycle.
 * Also T1 takes mutex m while "holding" s, and T2 later waits for s while
 * holding m: a cycle only if s were still tracked as a lock after the note.
 * Expected: one note, NO warning, normal exit.
 * (The semaphores go1/go2 start at 0: pure signals, never tracked.)
 */
#include "common.h"
#include <semaphore.h>

static sem_t s, t1_has_s, t2_done;
static pthread_mutex_t p, q, m;

static void *thread1(void *arg)
{
    (void)arg;
    sem_wait(&s);                 /* looks like taking a lock */
    pthread_mutex_lock(&m);       /* "s then m": an edge s -> m while s is tracked */
    pthread_mutex_unlock(&m);
    sem_post(&t1_has_s);          /* tell T2 */
    sem_wait(&t2_done);           /* wait until T2 is finished */
    pthread_mutex_lock(&q);       /* T1 holds nothing else: no edge, no cycle */
    pthread_mutex_unlock(&q);
    return NULL;
}

static void *thread2(void *arg)
{
    (void)arg;
    sem_wait(&t1_has_s);
    sem_post(&s);                 /* posts a semaphore it never waited on */
    pthread_mutex_lock(&m);       /* "m then s": would close a cycle with s -> m ... */
    sem_wait(&s);                 /* ... if s were still tracked as a lock */
    pthread_mutex_unlock(&m);
    pthread_mutex_lock(&p);       /* these two new locks reuse LockSight's freed ids */
    pthread_mutex_lock(&q);
    pthread_mutex_unlock(&q);
    pthread_mutex_unlock(&p);
    pthread_mutex_lock(&q);
    pthread_mutex_lock(&p);
    pthread_mutex_unlock(&p);
    pthread_mutex_unlock(&q);
    sem_post(&t2_done);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);
    sem_init(&s, 0, 1);
    sem_init(&t1_has_s, 0, 0);
    sem_init(&t2_done, 0, 0);
    pthread_mutex_init(&p, NULL);
    pthread_mutex_init(&q, NULL);
    pthread_mutex_init(&m, NULL);

    pthread_t a, b;
    pthread_create(&a, NULL, thread1, NULL);
    pthread_create(&b, NULL, thread2, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    finished("sem_handoff_ok");
    return 0;
}
