// cpp_mutex.cpp - the classic inversion written with C++ std::mutex.
// std::mutex calls pthread_mutex_lock underneath, so LockSight should still see it.
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>
#include <unistd.h>
static std::mutex A, B;
static bool lucky;
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--lucky")) lucky = true;
    std::thread wd([] { sleep(5); puts("\n[program] FROZEN: no progress after 5 seconds. The deadlock really happened."); fflush(stdout); _exit(124); });
    wd.detach();
    auto f1 = [] { std::lock_guard<std::mutex> a(A); usleep(lucky ? 0 : 200000); std::lock_guard<std::mutex> b(B); };
    auto f2 = [] { std::lock_guard<std::mutex> b(B); usleep(lucky ? 0 : 200000); std::lock_guard<std::mutex> a(A); };
    std::thread t1(f1);
    if (lucky) t1.join();
    std::thread t2(f2);
    if (!lucky) t1.join();
    t2.join();
    puts("cpp_mutex: finished");
}
