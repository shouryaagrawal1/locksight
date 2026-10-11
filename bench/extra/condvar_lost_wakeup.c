/* condvar_lost_wakeup.c - signal is sent before anyone waits, so the waiter sleeps forever.
 * A deadlock-like hang, but not a lock cycle: LockSight cannot see it. */
#include "../../tests/common.h"
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static void *waiter(void *x){ (void)x; sleep_ms(300);
    pthread_mutex_lock(&m); pthread_cond_wait(&cv, &m);      /* bug: no predicate loop */
    pthread_mutex_unlock(&m); return NULL; }
int main(int argc, char **argv){ parse_mode(argc, argv); start_watchdog(2);
    pthread_t a; pthread_create(&a, NULL, waiter, NULL);
    pthread_mutex_lock(&m); pthread_cond_signal(&cv); pthread_mutex_unlock(&m);   /* too early */
    pthread_join(a, NULL); printf("condvar_lost_wakeup: finished\n"); return 0; }
