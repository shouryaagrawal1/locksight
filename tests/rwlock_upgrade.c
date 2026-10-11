/*
 * rwlock_upgrade.c - a thread tries to "upgrade" its read lock to a write lock.
 *
 *   read-lock R, then write-lock R (without unlocking first)
 *
 * The write lock waits for all readers to leave, and this thread is one of
 * them: it waits for itself forever. LockSight must report a SELF-DEADLOCK.
 */
#include "common.h"

static pthread_rwlock_t r = PTHREAD_RWLOCK_INITIALIZER;

static void *worker(void *arg)
{
    (void)arg;
    pthread_rwlock_rdlock(&r);
    printf("[thread 1] has the read lock, now asks for the write lock\n");
    fflush(stdout);
    pthread_rwlock_wrlock(&r);          /* never returns */
    pthread_rwlock_unlock(&r);
    pthread_rwlock_unlock(&r);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_join(t, NULL);
    finished("rwlock_upgrade");
    return 0;
}
