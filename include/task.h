#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "arch_task.h"
#include "arch_vm.h"
#include "fs.h"

struct elf_info;
struct orth_runq_stat;

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
    struct cpu_local* self;
    uint32_t cpu_id;
    uint32_t reserved;
    struct task* current_task;
    struct task* idle_task;
    struct task* runq_head;
    struct task* runq_tail;
    volatile int resched_pending;
    uint32_t runq_count;
    uint32_t kernel_lock_depth;
};

typedef struct arch_task_context task_context_t;

struct task {
    uint64_t kstack_top;
    uint64_t os_stack_ptr;
    task_context_t ctx; // ctx offset = 16
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
    int cpu_affinity;
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
    uint64_t sleep_until_ms;
    int timeslice_ticks;
    char cwd[256];
    file_descriptor_t fds[MAX_FDS];
    struct task* next;
    struct task* runq_prev;
    struct task* runq_next;
    uint8_t on_runq;
};

void task_init(void);
void task_set_cpu_count(uint32_t cpu_count);
uint32_t task_get_cpu_count(void);
int task_get_runq_stats(struct orth_runq_stat* out, uint32_t max_count);
struct cpu_local* get_cpu_local_by_id(uint32_t cpu_id);
void task_bind_cpu_local(uint32_t cpu_id, struct task* current, struct task* idle,
                         uint64_t kernel_stack);
void task_install_cpu_local(uint32_t cpu_id);
struct task* task_create(uint64_t entry, uint64_t user_rsp);
struct task* task_create_on_cpu(uint64_t entry, uint64_t user_rsp, uint32_t cpu_id);
struct task* task_create_idle(uint32_t cpu_id);
int task_set_affinity(struct task* t, uint32_t cpu_id);
int task_mark_ready_on_cpu(struct task* t, uint32_t cpu_id);
int task_mark_sleeping(struct task* t);
int task_mark_zombie(struct task* t, int exit_status);
int task_wake(struct task* t);
int task_reap(struct task* t);
int task_set_fork_spread(int enabled);
int task_get_fork_spread(void);
void schedule(void);
struct cpu_local* get_cpu_local(void);
struct task* get_current_task(void);
task_context_t* task_current_context(void);
void task_request_resched(void);
void task_request_resched_cpu(uint32_t cpu_id);
int task_consume_resched(void);
void task_on_timer_tick(void);
int task_prepare_initial_user_stack(arch_address_space_t address_space, struct task* t,
                                    const struct elf_info* info,
                                    char* const argv[], char* const envp[]);
void task_idle_loop(int poll_network);

#endif
