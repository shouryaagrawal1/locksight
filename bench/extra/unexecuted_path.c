/* unexecuted_path.c - the bad lock order sits in a branch this run never takes.
 * Every dynamic tool (LockSight, TSan, Helgrind) can only see executed code. */
#include "../../tests/common.h"
static pthread_mutex_t A = PTHREAD_MUTEX_INITIALIZER, B = PTHREAD_MUTEX_INITIALIZER;
static int rare_flag;
static void *t1(void *x){ (void)x; pthread_mutex_lock(&A); pthread_mutex_lock(&B);
    pthread_mutex_unlock(&B); pthread_mutex_unlock(&A); return NULL; }
static void *t2(void *x){ (void)x;
    if (rare_flag) { pthread_mutex_lock(&B); pthread_mutex_lock(&A);   /* the bug: never executed */
        pthread_mutex_unlock(&A); pthread_mutex_unlock(&B); }
    else { pthread_mutex_lock(&A); pthread_mutex_lock(&B);
        pthread_mutex_unlock(&B); pthread_mutex_unlock(&A); }
    return NULL; }
int main(int argc, char **argv){ parse_mode(argc, argv); (void)g_lucky; start_watchdog(5);
    pthread_t a, b; pthread_create(&a, NULL, t1, NULL); pthread_join(a, NULL);
    pthread_create(&b, NULL, t2, NULL); pthread_join(b, NULL);
    printf("unexecuted_path: finished\n"); return 0; }
