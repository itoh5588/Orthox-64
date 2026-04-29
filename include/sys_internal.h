#ifndef SYS_INTERNAL_H
#define SYS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "syscall.h"

struct orth_timeval_k {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct orth_timespec_k {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_utsname_k {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct linux_rlimit_k {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct linux_sysinfo_k {
    unsigned long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
    char __reserved[256];
};

struct linux_stack_t_k {
    void* ss_sp;
    int ss_flags;
    size_t ss_size;
};

struct linux_rt_sigaction_k {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint32_t mask[2];
};

int sys_gettimeofday(struct orth_timeval_k* tv);
int sys_clock_gettime(int clock_id, struct orth_timespec_k* ts);
int sys_sched_yield(void);
int sys_nanosleep(const struct orth_timespec_k* req, struct orth_timespec_k* rem);
int sys_uname(struct linux_utsname_k* buf);
int sys_getrlimit(unsigned resource, struct linux_rlimit_k* rlim);
int sys_prlimit64(int pid, unsigned resource, const struct linux_rlimit_k* new_limit,
                  struct linux_rlimit_k* old_limit);
int sys_sysinfo(struct linux_sysinfo_k* info);
int sys_sleep_ms(uint64_t ms);
int sys_sigaltstack(const struct linux_stack_t_k* ss, struct linux_stack_t_k* old_ss);
int sys_sigprocmask(int how, const uint64_t* set, uint64_t* oldset);
int sys_rt_sigprocmask(int how, const uint64_t* set, uint64_t* oldset, size_t sigsetsize);
int sys_sigpending(uint64_t* set);
int sys_sigaction(int sig, const struct orth_sigaction* act, struct orth_sigaction* oldact);
int sys_rt_sigaction(int sig, const struct linux_rt_sigaction_k* act,
                     struct linux_rt_sigaction_k* oldact, size_t sigsetsize);
uint64_t sys_brk(uint64_t addr);
void* sys_mmap(void* addr, size_t length, int prot, int flags, int fd, int64_t offset);
int sys_munmap(void* addr, size_t length);
int sys_mprotect(void* addr, size_t length, int prot);
void* sys_mremap(void* old_addr, size_t old_len, size_t new_len, int flags, void* new_addr);
int sys_madvise(void* addr, size_t len, int advice);
uint64_t sys_getpid(void);
uint64_t sys_getppid(void);
uint64_t sys_getuid(void);
uint64_t sys_getgid(void);
uint64_t sys_geteuid(void);
uint64_t sys_getegid(void);
int sys_arch_prctl(int code, uint64_t addr);
int sys_futex(volatile int* uaddr, int op, int val);
int sys_set_robust_list(const void* head, size_t len);
int sys_set_tid_address(int* tidptr);
void sys_exit(int status);
int64_t sys_wait4(int pid, int* wstatus, int options);
int sys_kill(int pid, int sig);
int sys_getpgrp(void);
int sys_setpgid(int pid, int pgid);
int sys_setsid(void);
int sys_tcgetpgrp(int fd);
int sys_tcsetpgrp(int fd, int pgrp);

#endif
