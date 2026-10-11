/*
 * sem_binary_ok.c - binary semaphores used correctly as locks.
 *
 * Both threads take s1 then s2, always in the same order, and each thread
 * posts what it waited for. LockSight must stay silent and print no note
 * (the posts come from the thread that waited, so the semaphores really are
 * locks here).
 */
#include "common.h"
#include <semaphore.h>

static sem_t s1, s2;
static int counter;

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        sem_wait(&s1);
        sem_wait(&s2);
        counter++;
        sem_post(&s2);
        sem_post(&s1);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);
    sem_init(&s1, 0, 1);
    sem_init(&s2, 0, 1);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, worker, NULL);
    pthread_create(&t2, NULL, worker, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("[program] counter = %d (expected 2000)\n", counter);
    finished("sem_binary_ok");
    return 0;
}
