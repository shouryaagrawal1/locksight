/*
 * common.h - small helpers shared by all LockSight test programs.
 *
 * Every test program can run in two modes:
 *
 *   ./prog            "unlucky" mode: threads overlap, so the program
 *                     REALLY deadlocks (a watchdog prints FROZEN and exits).
 *
 *   ./prog --lucky    "lucky" mode: threads run one after another, so the
 *                     program finishes fine. The dangerous lock order still
 *                     exists in the code, which is exactly the situation
 *                     LockSight must catch BEFORE a real freeze happens.
 */
#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_lucky = 0;   /* 1 = run threads one after another (no real freeze) */

static inline void parse_mode(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lucky") == 0)
            g_lucky = 1;
    }
}

static inline void sleep_ms(int ms)
{
    usleep((useconds_t)ms * 1000);
}

/* Watchdog: if the program is still running after N seconds, we assume it is
 * frozen in a deadlock, say so, and exit with code 124 (same as `timeout`). */
static void *watchdog_main(void *arg)
{
    int seconds = *(int *)arg;
    sleep(seconds);
    printf("\n[program] FROZEN: no progress after %d seconds. "
           "The deadlock really happened.\n", seconds);
    fflush(stdout);
    _exit(124);
    return NULL;
}

static inline void start_watchdog(int seconds)
{
    static int secs;
    pthread_t t;
    secs = seconds;
    pthread_create(&t, NULL, watchdog_main, &secs);
    pthread_detach(t);
}

static inline void finished(const char *name)
{
    printf("[program] %s finished normally. No freeze this time.\n", name);
    fflush(stdout);
}

#endif /* COMMON_H */
