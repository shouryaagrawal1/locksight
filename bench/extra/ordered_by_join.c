/* ordered_by_join.c - opposite lock orders, but thread 2 only starts AFTER thread 1
 * has been joined, so they can never overlap: no deadlock is possible.
 * LockSight only looks at lock orders, not at thread start/join order, so it
 * warns anyway: a FALSE ALARM (a known limit of lock-order analysis). */
#include "../../tests/common.h"
static pthread_mutex_t A = PTHREAD_MUTEX_INITIALIZER, B = PTHREAD_MUTEX_INITIALIZER;
static void *t1(void *x){ (void)x; pthread_mutex_lock(&A); pthread_mutex_lock(&B);
    pthread_mutex_unlock(&B); pthread_mutex_unlock(&A); return NULL; }
static void *t2(void *x){ (void)x; pthread_mutex_lock(&B); pthread_mutex_lock(&A);
    pthread_mutex_unlock(&A); pthread_mutex_unlock(&B); return NULL; }
int main(int argc, char **argv){ parse_mode(argc, argv); start_watchdog(5);
    pthread_t a, b; pthread_create(&a, NULL, t1, NULL); pthread_join(a, NULL);   /* always sequential */
    pthread_create(&b, NULL, t2, NULL); pthread_join(b, NULL);
    printf("ordered_by_join: finished\n"); return 0; }
