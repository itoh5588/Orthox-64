#include <sys/signal.h>
#include <stdio.h>
#include <unistd.h>

int kill(pid_t pid, int sig);
int sigpending(sigset_t* set);
int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact);
#ifndef sigismember
#define sigismember(setp, sig) (((*(setp)) & (1UL << (sig))) != 0)
#endif

static void fail(const char* msg) {
    printf("sigactiontest: FAIL: %s\n", msg);
}

static void dummy_handler(int sig) {
    (void)sig;
}

int main(void) {
    struct sigaction oldact;
    struct sigaction act;
    struct sigaction cur;
    sigset_t pending = 0;

    printf("--- Sigaction Registration Test ---\n");

    act.sa_handler = dummy_handler;
    act.sa_mask = 0;
    act.sa_flags = 0x1234;
    if (sigaction(SIGUSR1, &act, &oldact) != 0) {
        fail("sigaction install failed");
        return 1;
    }
    cur.sa_handler = SIG_DFL;
    cur.sa_mask = 0;
    cur.sa_flags = 0;
    if (sigaction(SIGUSR1, 0, &cur) != 0) {
        fail("sigaction query failed");
        return 1;
    }
    printf("queried handler=%p flags=%x mask=%lu\n",
           (void*)cur.sa_handler, cur.sa_flags, (unsigned long)cur.sa_mask);
    if (cur.sa_handler != dummy_handler || cur.sa_flags != 0x1234) {
        fail("sigaction query mismatch");
        return 1;
    }

    act.sa_handler = SIG_IGN;
    act.sa_mask = 0;
    act.sa_flags = 0;
    if (sigaction(SIGUSR1, &act, 0) != 0) {
        fail("sigaction ignore install failed");
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
    printf("pending after SIG_IGN=%lu\n", (unsigned long)pending);
    if (sigismember(&pending, SIGUSR1)) {
        fail("ignored SIGUSR1 still pending");
        return 1;
    }

    printf("sigactiontest: PASS\n");
    return 0;
}
