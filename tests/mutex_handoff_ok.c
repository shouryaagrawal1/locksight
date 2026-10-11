/*
 * mutex_handoff_ok.c - a mutex locked by one thread and released by another.
 *
 * glibc accepts this for ordinary mutexes (a "hand-off"). LockSight keeps a
 * list of held locks per thread, so it must notice that the lock was released
 * by somebody else; otherwise it would think the first thread still holds it
 * and report a bogus SELF-DEADLOCK when that thread locks it again.
 * Expected: silent.
 */
#include "common.h"
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *releaser(void *x){ (void)x; pthread_mutex_unlock(&m); return NULL; }
int main(int argc, char **argv){ parse_mode(argc, argv); start_watchdog(5);
    pthread_mutex_lock(&m);
    pthread_t t; pthread_create(&t, NULL, releaser, NULL); pthread_join(t, NULL);
    pthread_mutex_lock(&m);              /* fine: the other thread released it */
    pthread_mutex_unlock(&m);
    printf("cross_thread_unlock: finished\n"); return 0; }
