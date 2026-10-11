/*
 * sem_binary_deadlock.c - AB-BA with two BINARY SEMAPHORES used as locks.
 *
 *   sem_init(&s1, 0, 1);  sem_init(&s2, 0, 1);      (value 1 = "unlocked")
 *   Thread 1: sem_wait(s1), then sem_wait(s2)
 *   Thread 2: sem_wait(s2), then sem_wait(s1)       <-- opposite order
 */
#include "common.h"
#include <semaphore.h>

static sem_t s1, s2;

static void *thread1(void *arg)
{
    (void)arg;
    sem_wait(&s1);
    sleep_ms(g_lucky ? 0 : 200);
    sem_wait(&s2);
    printf("[thread 1] has both semaphores\n");
    sem_post(&s2);
    sem_post(&s1);
    return NULL;
}

static void *thread2(void *arg)
{
    (void)arg;
    sem_wait(&s2);
    sleep_ms(g_lucky ? 0 : 200);
    sem_wait(&s1);
    printf("[thread 2] has both semaphores\n");
    sem_post(&s1);
    sem_post(&s2);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);
    sem_init(&s1, 0, 1);
    sem_init(&s2, 0, 1);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread1, NULL);
    if (g_lucky) pthread_join(t1, NULL);
    pthread_create(&t2, NULL, thread2, NULL);
    if (!g_lucky) pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    finished("sem_binary_deadlock");
    return 0;
}
