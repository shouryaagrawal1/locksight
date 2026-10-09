/*
 * dining_philosophers.c - the classic five-philosopher problem.
 *
 * Five philosophers sit around a table with one fork between each pair.
 * Everyone picks up the LEFT fork first, then the RIGHT fork.
 * If all five grab their left fork at the same moment, nobody can get a
 * right fork, and everyone waits forever.
 *
 * In lock terms the cycle is: fork0 -> fork1 -> fork2 -> fork3 -> fork4 -> fork0
 */
#include "common.h"

#define N 5

static pthread_mutex_t fork_lock[N];

static void *philosopher(void *arg)
{
    int id    = *(int *)arg;
    int left  = id;
    int right = (id + 1) % N;

    pthread_mutex_lock(&fork_lock[left]);
    sleep_ms(g_lucky ? 0 : 200);           /* everyone holds a left fork now */
    pthread_mutex_lock(&fork_lock[right]);

    printf("[philosopher %d] is eating\n", id);

    pthread_mutex_unlock(&fork_lock[right]);
    pthread_mutex_unlock(&fork_lock[left]);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);

    pthread_t t[N];
    int ids[N];

    for (int i = 0; i < N; i++)
        pthread_mutex_init(&fork_lock[i], NULL);

    for (int i = 0; i < N; i++) {
        ids[i] = i;
        pthread_create(&t[i], NULL, philosopher, &ids[i]);
        if (g_lucky) pthread_join(t[i], NULL);     /* lucky: one at a time */
    }
    if (!g_lucky)
        for (int i = 0; i < N; i++) pthread_join(t[i], NULL);

    finished("dining_philosophers");
    return 0;
}
