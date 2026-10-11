/* maxrss.c - runs a command and prints its peak memory (KB) on the last line of stderr:
 *   ./maxrss PROGRAM [ARGS...]      ->  "MAXRSS_KB 1234" */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    pid_t p = fork();
    if (p == 0) { execvp(argv[1], argv + 1); _exit(127); }
    int st; struct rusage ru;
    wait4(p, &st, 0, &ru);
    fprintf(stderr, "MAXRSS_KB %ld\n", ru.ru_maxrss);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}
