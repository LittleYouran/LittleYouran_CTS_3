#include "scheduler.hpp"
#include <signal.h>

static void sigHandler(int sig) {
    fprintf(stderr, "收到信号 %d, 忽略\n", sig);
}

int main(void) {
    signal(SIGTERM, sigHandler);
    signal(SIGPIPE, SIG_IGN);
    Schedule sched;
    while (true) {
        sleep(3600 * 24 * 365);
    } 
}