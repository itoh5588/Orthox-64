#include <sys/signal.h>
#include <stdio.h>
#include <unistd.h>

static void fail(const char* msg) {
    printf("sigmasktest: FAIL: %s\n", msg);
}

int kill(pid_t pid, int sig);
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset);
int sigpending(sigset_t* set);
#ifndef SIG_SETMASK
#define SIG_SETMASK 0
#define SIG_BLOCK 1
#define SIG_UNBLOCK 2
#endif
#ifndef sigaddset
#define sigaddset(setp, sig) (*(setp) |= (1UL << (sig)), 0)
#define sigemptyset(setp) (*(setp) = 0, 0)
#define sigismember(setp, sig) (((*(setp)) & (1UL << (sig))) != 0)
#endif

int main(void) {
    sigset_t set = 0;
    sigset_t old = 0;
    sigset_t pending = 0;

    printf("--- Signal Mask / Pending Test ---\n");

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &set, &old) != 0) {
        fail("sigprocmask(SIG_BLOCK) failed");
        return 1;
    }
    if (kill(getpid(), SIGUSR1) != 0) {
        fail("kill(SIGUSR1) failed");
        return 1;
    }
    if (sigpending(&pending) != 0) {
        fail("sigpending failed");
        return 1;
    }
    printf("pending after block=%lu\n", (unsigned long)pending);
    if (!sigismember(&pending, SIGUSR1)) {
        fail("SIGUSR1 not reported pending while blocked");
        return 1;
    }

    if (sigprocmask(SIG_SETMASK, &old, 0) != 0) {
        fail("sigprocmask(SIG_SETMASK restore) failed");
        return 1;
    }
    pending = 0;
    if (sigpending(&pending) != 0) {
        fail("sigpending after restore failed");
        return 1;
    }
    printf("pending after restore=%lu\n", (unsigned long)pending);
    if (sigismember(&pending, SIGUSR1)) {
        fail("SIGUSR1 still reported pending after unmask");
        return 1;
    }

    printf("sigmasktest: PASS\n");
    return 0;
}
