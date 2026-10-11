/* rwlock_inversion.c - lock-order inversion with pthread_rwlock_t (write locks).
 * LockSight only intercepts mutexes, so it is expected to MISS this one. */
#include "../../tests/common.h"
static pthread_rwlock_t A = PTHREAD_RWLOCK_INITIALIZER, B = PTHREAD_RWLOCK_INITIALIZER;
static void *t1(void *x){ (void)x; pthread_rwlock_wrlock(&A); sleep_ms(g_lucky?0:200); pthread_rwlock_wrlock(&B);
    pthread_rwlock_unlock(&B); pthread_rwlock_unlock(&A); return NULL; }
static void *t2(void *x){ (void)x; pthread_rwlock_wrlock(&B); sleep_ms(g_lucky?0:200); pthread_rwlock_wrlock(&A);
    pthread_rwlock_unlock(&A); pthread_rwlock_unlock(&B); return NULL; }
int main(int argc, char **argv){ parse_mode(argc, argv); start_watchdog(5);
    pthread_t a, b; pthread_create(&a, NULL, t1, NULL); if (g_lucky) pthread_join(a, NULL);
    pthread_create(&b, NULL, t2, NULL); if (!g_lucky) pthread_join(a, NULL); pthread_join(b, NULL);
    printf("rwlock_inversion: finished\n"); return 0; }
