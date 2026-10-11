/* semaphore_deadlock.c - two threads each wait for a semaphore the other must post.
 * Not a lock-order problem and no mutex is involved: LockSight cannot see it. */
#include "../../tests/common.h"
#include <semaphore.h>
static sem_t s1, s2;
static void *t1(void *x){ (void)x; sem_wait(&s1); sem_post(&s2); return NULL; }
static void *t2(void *x){ (void)x; sem_wait(&s2); sem_post(&s1); return NULL; }
int main(int argc, char **argv){ parse_mode(argc, argv); start_watchdog(2);
    sem_init(&s1, 0, 0); sem_init(&s2, 0, 0);
    pthread_t a, b; pthread_create(&a, NULL, t1, NULL); pthread_create(&b, NULL, t2, NULL);
    pthread_join(a, NULL); pthread_join(b, NULL); printf("semaphore_deadlock: finished\n"); return 0; }
