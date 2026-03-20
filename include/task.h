#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "fs.h"

struct elf_info;

typedef enum {
    TASK_RUNNING,
    TASK_READY,
    TASK_SLEEPING,
    TASK_ZOMBIE,
    TASK_DEAD
} task_state_t;

struct task;

#define ORTHOX_MAX_CPUS 64

struct cpu_local {
    uint64_t kernel_stack;
    uint64_t user_stack;
    uint32_t cpu_id;
    uint32_t reserved;
    struct task* current_task;
    struct task* idle_task;
    volatile int resched_pending;
};

struct task_context {
    uint64_t cr3, rip, rflags, reserved1; // 0, 8, 16, 24
    uint64_t cs, ss, fs, gs;             // 32, 40, 48, 56
    uint64_t rax, rbx, rcx, rdx, rdi, rsi, rsp, rbp; // 64, 72, 80, 88, 96, 104, 112, 120
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;   // 128, 136, 144, 152, 160, 168, 176, 184
    uint8_t fxsave_area[512] __attribute__((aligned(16))); // 192
} __attribute__((packed));

struct task {
    uint64_t kstack_top;
    uint64_t os_stack_ptr;
    struct task_context ctx; // ctx offset = 16
    int pid;
    int ppid;
    int pgid;
    int sid;
    uint64_t sig_pending;
    uint64_t sig_mask;
    uint64_t sig_handlers[32];
    uint64_t sig_action_masks[32];
    uint32_t sig_action_flags[32];
    int exit_status;
    task_state_t state;
    uint64_t heap_break;
    uint64_t mmap_end;
    uint64_t user_entry;
    uint64_t user_stack;
    uint64_t user_stack_top;
    uint64_t user_stack_bottom;
    uint64_t user_stack_guard;
    uint64_t user_argc;
    uint64_t user_argv;
    uint64_t user_envp;
    uint64_t user_fs_base;
    int timeslice_ticks;
    char cwd[256];
    file_descriptor_t fds[MAX_FDS];
    struct task* next;
};

void task_init(void);
void task_set_cpu_count(uint32_t cpu_count);
uint32_t task_get_cpu_count(void);
struct cpu_local* get_cpu_local_by_id(uint32_t cpu_id);
struct task* task_create(uint64_t entry, uint64_t user_rsp);
void schedule(void);
struct cpu_local* get_cpu_local(void);
struct task* get_current_task(void);
void task_request_resched(void);
int task_consume_resched(void);
void task_on_timer_tick(void);
int task_prepare_initial_user_stack(uint64_t* pml4_virt, struct task* t,
                                    const struct elf_info* info,
                                    char* const argv[], char* const envp[]);

#endif
