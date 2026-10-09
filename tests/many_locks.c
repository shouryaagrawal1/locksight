/*
 * many_locks.c - stress test. LockSight must stay silent and must not run out
 * of table space.
 *
 * Four threads keep creating and destroying objects that each contain two
 * mutexes (always locked in the same order). Memory is freed and reused all
 * the time, so the same addresses keep coming back. If LockSight did not
 * recycle its lock and edge records, its tables would fill up; if it
 * mixed up old and new mutexes, it would print false warnings.
 */
#include "common.h"

#define THREADS 4
#define POOL    64
#define ITER    30000

typedef struct {
    pthread_mutex_t a;
    pthread_mutex_t b;
    char            pad[16];
} Obj;

static void destroy_obj(Obj *o)
{
    pthread_mutex_destroy(&o->a);
    pthread_mutex_destroy(&o->b);
    free(o);
}

static void *worker(void *arg)
{
    unsigned seed = (unsigned)(size_t)arg;
    Obj *pool[POOL] = { 0 };

    for (int i = 0; i < ITER; i++) {
        int k = (int)(rand_r(&seed) % POOL);
        if (pool[k]) {
            destroy_obj(pool[k]);
            pool[k] = NULL;
        } else {
            Obj *o = malloc(sizeof(Obj) + rand_r(&seed) % 200);   /* varied sizes */
            pthread_mutex_init(&o->a, NULL);
            pthread_mutex_init(&o->b, NULL);
            pthread_mutex_lock(&o->a);            /* always a, then b */
            pthread_mutex_lock(&o->b);
            pthread_mutex_unlock(&o->b);
            pthread_mutex_unlock(&o->a);
            pool[k] = o;
        }
    }
    for (int k = 0; k < POOL; k++)
        if (pool[k])
            destroy_obj(pool[k]);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(60);

    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, worker, (void *)(size_t)(i + 1));
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);

    finished("many_locks");
    return 0;
}
