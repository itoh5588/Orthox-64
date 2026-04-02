#include <stdint.h>
#include <stddef.h>
#include "arch_entry.h"
#include "arch_time.h"
#include "arch_vm.h"
#include "task.h"
#include "syscall.h"
#include "pmm.h"
#include "vmm.h"
#include "limine.h"
#include "elf64.h"
#include "net_socket.h"
#include "net.h"
#include "smp.h"
#include "spinlock.h"

static struct cpu_local g_cpu_locals[ORTHOX_MAX_CPUS];
struct task* task_list = NULL;
static int next_pid = 1;
static uint32_t g_cpu_count = 1;
static spinlock_t g_task_lock;
static int g_fork_spread_enabled = 1;

#define TASK_TIMESLICE_TICKS 5

#define USER_STACK_TOP_VADDR   0x7FFFFFFFF000ULL
#define USER_STACK_PAGES       64
#define USER_STACK_GUARD_PAGES 1
#define EXEC_COPY_PAGES        4
#define EXEC_MAX_PATH_LEN      1024
#define EXEC_MAX_VEC_STRINGS   32
#define EXEC_MAX_VEC_STR_LEN   160

extern volatile struct limine_module_request module_request;

extern void puts(const char* s);
extern void puthex(uint64_t v);
static int free_task_struct(struct task* t);
static int task_is_idle_task(struct task* t);
static int task_can_migrate_locked(struct task* t);
static int normalize_cpu_affinity(uint32_t cpu_id);
static void task_runq_push_locked(struct task* t, uint32_t cpu_id);

enum task_migration_reason {
    TASK_MIGRATE_OK = 0,
    TASK_MIGRATE_BLOCK_NULL,
    TASK_MIGRATE_BLOCK_IDLE,
    TASK_MIGRATE_BLOCK_NOT_READY,
    TASK_MIGRATE_BLOCK_NOT_ON_RUNQ,
};

static enum task_migration_reason task_migration_reason_locked(struct task* t);

static void idle_task_entry(void) {
    task_idle_loop(0);
}

static void task_init_idle_context(struct task* t) {
    if (!t || !task_is_idle_task(t)) return;
    t->os_stack_ptr = arch_task_prepare_kernel_entry(
        &t->ctx,
        t->kstack_top,
        (uint64_t)idle_task_entry,
        arch_task_context_get_address_space(&t->ctx));
}

struct exec_copy_buf {
    char path[EXEC_MAX_PATH_LEN];
    char argv_storage[EXEC_MAX_VEC_STRINGS][EXEC_MAX_VEC_STR_LEN];
    char envp_storage[EXEC_MAX_VEC_STRINGS][EXEC_MAX_VEC_STR_LEN];
    char* argv[EXEC_MAX_VEC_STRINGS + 1];
    char* envp[EXEC_MAX_VEC_STRINGS + 1];
};

static int copy_user_cstring(const char* src, char* dst, int size) {
    if (!src || !dst || size <= 0) return -1;
    if ((uint64_t)src < 0x1000ULL) return -1;
    int i = 0;
    for (; i + 1 < size; i++) {
        char ch = src[i];
        dst[i] = ch;
        if (!ch) return 0;
    }
    dst[size - 1] = '\0';
    return -1;
}

static int copy_user_string_vector(char* const user_vec[], char** kernel_vec,
                                   char storage[][EXEC_MAX_VEC_STR_LEN]) {
    int count = 0;
    if (!kernel_vec) return -1;
    if (!user_vec) {
        kernel_vec[0] = 0;
        return 0;
    }
    while (count < EXEC_MAX_VEC_STRINGS) {
        const char* src = user_vec[count];
        if (!src) break;
        if (copy_user_cstring(src, storage[count], EXEC_MAX_VEC_STR_LEN) < 0) {
            return -1;
        }
        kernel_vec[count] = storage[count];
        count++;
    }
    kernel_vec[count] = 0;
    return 0;
}

static inline struct cpu_local* this_cpu(void) {
    return arch_task_get_cpu_local();
}

struct cpu_local* get_cpu_local(void) {
    return this_cpu();
}

struct cpu_local* get_cpu_local_by_id(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count || cpu_id >= ORTHOX_MAX_CPUS) return NULL;
    return &g_cpu_locals[cpu_id];
}

void task_set_cpu_count(uint32_t cpu_count) {
    if (cpu_count == 0) cpu_count = 1;
    if (cpu_count > ORTHOX_MAX_CPUS) cpu_count = ORTHOX_MAX_CPUS;
    g_cpu_count = cpu_count;
}

uint32_t task_get_cpu_count(void) {
    return g_cpu_count;
}

int task_get_runq_stats(struct orth_runq_stat* out, uint32_t max_count) {
    uint32_t cpu_count = task_get_cpu_count();
    uint32_t started = smp_get_started_cpu_count();
    uint32_t count = 0;
    uint64_t flags;

    if (!out || max_count == 0) return -1;
    if (cpu_count == 0) cpu_count = 1;
    if (started == 0 || started > cpu_count) started = cpu_count;

    flags = spin_lock_irqsave(&g_task_lock);
    for (uint32_t cpu_id = 0; cpu_id < started && count < max_count; cpu_id++) {
        struct cpu_local* cpu = get_cpu_local_by_id(cpu_id);
        struct task* current;
        uint32_t migratable = 0;
        uint32_t affined_tasks = 0;
        uint32_t affined_ready = 0;
        uint32_t affined_running = 0;
        uint32_t affined_sleeping = 0;
        uint32_t blocked_ready = 0;
        uint32_t blocked_running = 0;
        uint32_t blocked_sleeping = 0;
        if (!cpu) continue;
        current = cpu->current_task;
        for (struct task* t = task_list; t; t = t->next) {
            enum task_migration_reason migrate_reason;
            if (task_is_idle_task(t)) continue;
            if (t->state == TASK_DEAD) continue;
            if ((uint32_t)normalize_cpu_affinity((uint32_t)t->cpu_affinity) != cpu_id) continue;
            affined_tasks++;
            migrate_reason = task_migration_reason_locked(t);
            if (t->state == TASK_READY) {
                affined_ready++;
                if (migrate_reason == TASK_MIGRATE_OK) migratable++;
                else blocked_ready++;
            } else if (t->state == TASK_RUNNING) {
                affined_running++;
                if (migrate_reason != TASK_MIGRATE_OK) blocked_running++;
            } else if (t->state == TASK_SLEEPING) {
                affined_sleeping++;
                if (migrate_reason != TASK_MIGRATE_OK) blocked_sleeping++;
            }
        }
        out[count].cpu_id = cpu_id;
        out[count].runq_count = cpu->runq_count;
        out[count].total_load = cpu->runq_count;
        out[count].affined_tasks = affined_tasks;
        out[count].affined_ready = affined_ready;
        out[count].affined_running = affined_running;
        out[count].affined_sleeping = affined_sleeping;
        out[count].blocked_ready = blocked_ready;
        out[count].blocked_running = blocked_running;
        out[count].blocked_sleeping = blocked_sleeping;
        out[count].current_pid = current ? current->pid : -1;
        out[count].current_state = current ? (int32_t)current->state : -1;
        out[count].runq_head_pid = cpu->runq_head ? cpu->runq_head->pid : -1;
        out[count].runq_tail_pid = cpu->runq_tail ? cpu->runq_tail->pid : -1;
        out[count].current_is_idle = (current && current == cpu->idle_task) ? 1U : 0U;
        out[count].migratable_count = migratable;
        if (current && current != cpu->idle_task && current->state == TASK_RUNNING) {
            out[count].total_load++;
        }
        count++;
    }
    spin_unlock_irqrestore(&g_task_lock, flags);
    return (int)count;
}

static inline struct task* get_current_task_raw(void) {
    struct cpu_local* cpu = this_cpu();
    return cpu ? cpu->current_task : NULL;
}

static int default_task_cpu_affinity(void) {
    struct cpu_local* cpu = this_cpu();
    return cpu ? (int)cpu->cpu_id : 0;
}

static int normalize_cpu_affinity(uint32_t cpu_id) {
    uint32_t cpu_count = task_get_cpu_count();
    if (cpu_count == 0) cpu_count = 1;
    if (cpu_id >= cpu_count) return 0;
    return (int)cpu_id;
}

static uint32_t task_cpu_load_locked(uint32_t cpu_id) {
    struct cpu_local* cpu = get_cpu_local_by_id(cpu_id);
    uint32_t load = 0;
    if (!cpu) return UINT32_MAX;
    load = cpu->runq_count;
    if (cpu->current_task && cpu->current_task != cpu->idle_task &&
        cpu->current_task->state == TASK_RUNNING) {
        load++;
    }
    return load;
}

static uint32_t task_choose_rebalance_cpu_locked(uint32_t fallback_cpu) {
    uint32_t cpu_count = task_get_cpu_count();
    uint32_t started = smp_get_started_cpu_count();
    uint32_t best_cpu;
    uint32_t best_load;

    if (cpu_count == 0) cpu_count = 1;
    if (started == 0 || started > cpu_count) started = cpu_count;
    best_cpu = (uint32_t)normalize_cpu_affinity(fallback_cpu);
    best_load = task_cpu_load_locked(best_cpu);

    for (uint32_t cpu_id = 0; cpu_id < started; cpu_id++) {
        const struct smp_cpu_info* cpu = smp_get_cpu_info(cpu_id);
        if (cpu && cpu->started) {
            uint32_t load = task_cpu_load_locked(cpu_id);
            if (load < best_load) {
                best_load = load;
                best_cpu = cpu_id;
            }
        }
    }
    return best_cpu;
}

static uint32_t task_rebalance_ready_task_locked(struct task* t) {
    uint32_t current_cpu;
    uint32_t best_cpu;
    uint32_t current_load;
    uint32_t best_load;

    if (!task_can_migrate_locked(t)) {
        return t ? (uint32_t)normalize_cpu_affinity((uint32_t)t->cpu_affinity) : 0;
    }

    current_cpu = (uint32_t)normalize_cpu_affinity((uint32_t)t->cpu_affinity);
    best_cpu = task_choose_rebalance_cpu_locked(current_cpu);
    if (best_cpu == current_cpu) return current_cpu;

    current_load = task_cpu_load_locked(current_cpu);
    best_load = task_cpu_load_locked(best_cpu);
    if (current_load <= best_load + 1) return current_cpu;

    task_runq_push_locked(t, best_cpu);
    return (uint32_t)normalize_cpu_affinity((uint32_t)t->cpu_affinity);
}

static uint32_t choose_spawn_cpu_locked(uint32_t fallback_cpu) {
    static uint32_t next_cpu = 0;
    uint32_t cpu_count = task_get_cpu_count();
    uint32_t started = smp_get_started_cpu_count();
    uint32_t best_cpu;
    uint32_t best_load;
    if (cpu_count == 0) cpu_count = 1;
    if (started == 0 || started > cpu_count) started = cpu_count;
    if (started <= 1) return (uint32_t)normalize_cpu_affinity(fallback_cpu);

    best_cpu = (uint32_t)normalize_cpu_affinity(fallback_cpu);
    best_load = task_cpu_load_locked(best_cpu);
    uint32_t start = next_cpu;
    for (uint32_t attempt = 0; attempt < started; attempt++) {
        uint32_t cpu_id = (start + attempt) % started;
        const struct smp_cpu_info* cpu = smp_get_cpu_info(cpu_id);
        if (cpu && cpu->started) {
            uint32_t load = task_cpu_load_locked(cpu_id);
            if (load < best_load) {
                best_load = load;
                best_cpu = cpu_id;
            }
        }
    }
    next_cpu = (best_cpu + 1) % started;
    return best_cpu;
}

int task_set_fork_spread(int enabled) {
    g_fork_spread_enabled = enabled ? 1 : 0;
    return 0;
}

int task_get_fork_spread(void) {
    return g_fork_spread_enabled;
}

static void init_cpu_local(struct cpu_local* cpu, uint32_t cpu_id,
                           struct task* current, struct task* idle,
                           uint64_t kernel_stack) {
    if (!cpu) return;
    cpu->cpu_id = cpu_id;
    cpu->self = cpu;
    cpu->current_task = current;
    cpu->idle_task = idle;
    cpu->runq_head = 0;
    cpu->runq_tail = 0;
    cpu->resched_pending = 0;
    cpu->runq_count = 0;
    cpu->kernel_lock_depth = 0;
    cpu->kernel_stack = kernel_stack;
    cpu->user_stack = 0;
}

static void task_refresh_cpu_local_msrs(struct cpu_local* cpu) {
    if (!cpu) return;
    arch_task_set_cpu_local(cpu);
}

static struct cpu_local* task_get_ready_cpu_locked(uint32_t cpu_id) {
    return get_cpu_local_by_id(normalize_cpu_affinity(cpu_id));
}

static int task_is_idle_task(struct task* t) {
    return t && t->pid == 0;
}

static enum task_migration_reason task_migration_reason_locked(struct task* t) {
    if (!t) return TASK_MIGRATE_BLOCK_NULL;
    if (task_is_idle_task(t)) return TASK_MIGRATE_BLOCK_IDLE;
    if (t->state != TASK_READY) return TASK_MIGRATE_BLOCK_NOT_READY;
    if (!t->on_runq) return TASK_MIGRATE_BLOCK_NOT_ON_RUNQ;
    return TASK_MIGRATE_OK;
}

static int task_can_migrate_locked(struct task* t) {
    return task_migration_reason_locked(t) == TASK_MIGRATE_OK;
}

static void task_runq_remove_locked(struct task* t) {
    struct cpu_local* cpu;
    if (!t || !t->on_runq) return;
    cpu = task_get_ready_cpu_locked((uint32_t)t->cpu_affinity);
    if (!cpu) return;
    if (t->runq_prev) t->runq_prev->runq_next = t->runq_next;
    else cpu->runq_head = t->runq_next;
    if (t->runq_next) t->runq_next->runq_prev = t->runq_prev;
    else cpu->runq_tail = t->runq_prev;
    t->runq_prev = 0;
    t->runq_next = 0;
    t->on_runq = 0;
    if (cpu->runq_count > 0) cpu->runq_count--;
}

static void task_runq_push_locked(struct task* t, uint32_t cpu_id) {
    struct cpu_local* cpu;
    if (!t || task_is_idle_task(t)) return;
    cpu_id = (uint32_t)normalize_cpu_affinity(cpu_id);
    if (t->on_runq) task_runq_remove_locked(t);
    cpu = task_get_ready_cpu_locked(cpu_id);
    if (!cpu) return;
    t->cpu_affinity = (int)cpu_id;
    t->runq_prev = cpu->runq_tail;
    t->runq_next = 0;
    if (cpu->runq_tail) cpu->runq_tail->runq_next = t;
    else cpu->runq_head = t;
    cpu->runq_tail = t;
    t->on_runq = 1;
    cpu->runq_count++;
}

static struct task* task_runq_pop_locked(struct cpu_local* cpu) {
    struct task* t;
    if (!cpu) return 0;
    t = cpu->runq_head;
    if (!t) return 0;
    task_runq_remove_locked(t);
    return t;
}

static int task_set_affinity_locked(struct task* t, uint32_t cpu_id) {
    if (!t) return -1;
    cpu_id = (uint32_t)normalize_cpu_affinity(cpu_id);
    if (!task_can_migrate_locked(t)) {
        if (t->cpu_affinity == (int)cpu_id) return 0;
        return -1;
    }
    task_runq_push_locked(t, cpu_id);
    return 0;
}

static int task_mark_ready_on_cpu_locked(struct task* t, uint32_t cpu_id) {
    if (!t) return -1;
    if (task_is_idle_task(t)) return -1;
    if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD) return -1;
    t->state = TASK_READY;
    t->sleep_until_ms = 0;
    task_runq_push_locked(t, cpu_id);
    return 0;
}

static int task_mark_sleeping_locked(struct task* t) {
    if (!t) return -1;
    task_runq_remove_locked(t);
    t->sleep_until_ms = 0;
    t->state = TASK_SLEEPING;
    return 0;
}

static int task_mark_zombie_locked(struct task* t, int exit_status) {
    if (!t) return -1;
    task_runq_remove_locked(t);
    t->exit_status = exit_status;
    t->state = TASK_ZOMBIE;
    return 0;
}

static int task_wake_locked(struct task* t) {
    if (!t) return -1;
    if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD) return -1;
    t->sleep_until_ms = 0;
    return task_mark_ready_on_cpu_locked(t, (uint32_t)t->cpu_affinity);
}

static int task_reap_locked(struct task* t) {
    struct task** link;
    if (!t) return -1;
    if (t->state != TASK_ZOMBIE && t->state != TASK_DEAD) return -1;
    task_runq_remove_locked(t);

    link = &task_list;
    while (*link && *link != t) {
        link = &(*link)->next;
    }
    if (*link != t) return -1;
    *link = t->next;

    t->state = TASK_DEAD;

    uint64_t address_space = arch_task_context_get_address_space(&t->ctx);
    if (address_space && address_space != arch_vm_kernel_address_space()) {
        arch_vm_destroy_user_address_space(address_space);
        arch_task_context_set_address_space(&t->ctx, 0);
    }

    if (t->kstack_top) {
        uint64_t kstack_phys = VIRT_TO_PHYS((void*)(t->kstack_top - 4 * PAGE_SIZE));
        pmm_free((void*)kstack_phys, 4);
        t->kstack_top = 0;
    }

    t->next = 0;
    t->runq_prev = 0;
    t->runq_next = 0;
    t->on_runq = 0;
    return free_task_struct(t);
}

void task_bind_cpu_local(uint32_t cpu_id, struct task* current, struct task* idle,
                         uint64_t kernel_stack) {
    struct cpu_local* cpu = get_cpu_local_by_id(cpu_id);
    if (!cpu) return;
    init_cpu_local(cpu, cpu_id, current, idle, kernel_stack);
}

void task_install_cpu_local(uint32_t cpu_id) {
    struct cpu_local* cpu = get_cpu_local_by_id(cpu_id);
    task_refresh_cpu_local_msrs(cpu);
}

struct task* get_current_task(void) {
    return get_current_task_raw();
}

task_context_t* task_current_context(void) {
    struct task* t = get_current_task_raw();
    return t ? &t->ctx : 0;
}

void task_request_resched(void) {
    struct cpu_local* cpu = this_cpu();
    if (cpu) cpu->resched_pending = 1;
}

void task_request_resched_cpu(uint32_t cpu_id) {
    struct cpu_local* cpu = get_cpu_local_by_id(cpu_id);
    struct cpu_local* self = this_cpu();
    if (!cpu) return;
    cpu->resched_pending = 1;
    if (!self || self->cpu_id != cpu_id) {
        smp_send_resched_ipi(cpu_id);
    }
}

int task_consume_resched(void) {
    struct cpu_local* cpu = this_cpu();
    if (!cpu || !cpu->resched_pending) return 0;
    cpu->resched_pending = 0;
    return 1;
}

void task_on_timer_tick(void) {
    struct task* current_task = get_current_task_raw();
    struct cpu_local* cpu = this_cpu();
    if (cpu && cpu->cpu_id == 0) {
        uint64_t now = arch_time_now_ms();
        uint64_t flags = spin_lock_irqsave(&g_task_lock);
        struct task* t = task_list;
        while (t) {
            if (t->state == TASK_SLEEPING && t->sleep_until_ms != 0 && t->sleep_until_ms <= now) {
                t->sleep_until_ms = 0;
                task_wake_locked(t);
            }
            t = t->next;
        }
        spin_unlock_irqrestore(&g_task_lock, flags);
    }
    if (!current_task || current_task->state != TASK_RUNNING) return;
    if (current_task->timeslice_ticks > 1) {
        current_task->timeslice_ticks--;
        return;
    }
    current_task->timeslice_ticks = TASK_TIMESLICE_TICKS;
    task_request_resched();
}

static void* kernel_memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static void kernel_strcpy(char* dst, const char* src, size_t size) {
    size_t i = 0;
    if (!dst || size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i + 1 < size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static struct arch_task_user_state task_user_state(const struct task* t) {
    struct arch_task_user_state state;
    state.entry_pc = t ? t->user_entry : 0;
    state.user_sp = t ? t->user_stack : 0;
    state.arg0 = t ? t->user_argc : 0;
    state.arg1 = t ? t->user_argv : 0;
    state.arg2 = t ? t->user_envp : 0;
    return state;
}

static void task_set_user_state(struct task* t, const struct arch_task_user_state* state) {
    if (!t || !state) return;
    t->user_entry = state->entry_pc;
    t->user_stack = state->user_sp;
    t->user_argc = state->arg0;
    t->user_argv = state->arg1;
    t->user_envp = state->arg2;
    arch_task_sync_user_state(&t->ctx, state);
}

static void task_init_user_layout(struct task* t) {
    if (!t) return;
    t->user_stack_top = USER_STACK_TOP_VADDR;
    t->user_stack_bottom = 0;
    t->user_stack_guard = 0;
}

static void task_copy_user_layout(struct task* dst, const struct task* src) {
    if (!dst || !src) return;
    dst->user_stack_top = src->user_stack_top;
    dst->user_stack_bottom = src->user_stack_bottom;
    dst->user_stack_guard = src->user_stack_guard;
}

static void task_copy_user_state(struct task* dst, const struct task* src) {
    struct arch_task_user_state state;
    if (!dst || !src) return;
    state = task_user_state(src);
    task_set_user_state(dst, &state);
}

static void task_install_exec_image(struct task* t,
                                    arch_address_space_t address_space,
                                    const struct elf_info* info) {
    struct arch_task_user_state state;
    if (!t || !info) return;
    arch_task_context_set_address_space(&t->ctx, address_space);
    t->heap_break = info->max_vaddr;
    t->mmap_end = 0x4000000000ULL;
    t->user_fs_base = 0;
    state = task_user_state(t);
    state.entry_pc = (uint64_t)info->entry;
    task_set_user_state(t, &state);
}

static void init_console_fds(struct task* t) {
    if (!t) return;
    t->fds[0].type = FT_CONSOLE;
    t->fds[0].in_use = 1;
    t->fds[0].flags = O_RDONLY;
    t->fds[1].type = FT_CONSOLE;
    t->fds[1].in_use = 1;
    t->fds[1].flags = O_WRONLY;
    t->fds[2].type = FT_CONSOLE;
    t->fds[2].in_use = 1;
    t->fds[2].flags = O_WRONLY;
}

static struct task* alloc_task_struct(void) {
    int task_pages = (sizeof(struct task) + PAGE_SIZE - 1) / PAGE_SIZE;
    void* phys = pmm_alloc(task_pages);
    if (!phys) return NULL;
    struct task* t = (struct task*)PHYS_TO_VIRT(phys);
    kernel_memset(t, 0, sizeof(struct task));
    return t;
}

static int free_task_struct(struct task* t) {
    if (!t) return -1;
    int task_pages = (sizeof(struct task) + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_free((void*)VIRT_TO_PHYS(t), task_pages);
    return 0;
}

static int alloc_user_stack(arch_address_space_t address_space, struct task* t, int stack_pages, uint8_t* stack_pages_out[]) {
    if (!address_space || !t || stack_pages <= USER_STACK_GUARD_PAGES) return -1;
    uint64_t stack_bottom_vaddr = USER_STACK_TOP_VADDR - (uint64_t)stack_pages * PAGE_SIZE;
    uint64_t mapped_bottom_vaddr = stack_bottom_vaddr + USER_STACK_GUARD_PAGES * PAGE_SIZE;
    for (int i = 0; i < stack_pages - USER_STACK_GUARD_PAGES; i++) {
        void* stack_phys = pmm_alloc(1);
        if (!stack_phys) return -1;
        uint8_t* stack_mem = (uint8_t*)PHYS_TO_VIRT(stack_phys);
        kernel_memset(stack_mem, 0, PAGE_SIZE);
        arch_vm_map_page(address_space,
                         mapped_bottom_vaddr + (uint64_t)i * PAGE_SIZE,
                         (uint64_t)stack_phys,
                         arch_vm_user_page_flags(1, 0));
        if (stack_pages_out) stack_pages_out[i] = stack_mem;
    }
    t->user_stack_top = USER_STACK_TOP_VADDR;
    t->user_stack_bottom = mapped_bottom_vaddr;
    t->user_stack_guard = stack_bottom_vaddr;
    return 0;
}

static int stack_write_bytes(uint8_t* stack_pages[], uint64_t mapped_bottom_vaddr,
                             uint64_t stack_top_vaddr, uint64_t user_addr,
                             const void* src, int len) {
    const uint8_t* in = (const uint8_t*)src;
    if (!stack_pages || !src || len < 0) return -1;
    if (user_addr < mapped_bottom_vaddr || user_addr + (uint64_t)len > stack_top_vaddr) return -1;
    uint64_t rel = user_addr - mapped_bottom_vaddr;
    int remaining = len;
    while (remaining > 0) {
        uint64_t page_index = rel / PAGE_SIZE;
        uint64_t page_off = rel % PAGE_SIZE;
        int chunk = (int)(PAGE_SIZE - page_off);
        if (chunk > remaining) chunk = remaining;
        if (!stack_pages[page_index]) return -1;
        for (int i = 0; i < chunk; i++) {
            stack_pages[page_index][page_off + (uint64_t)i] = in[i];
        }
        in += chunk;
        rel += (uint64_t)chunk;
        remaining -= chunk;
    }
    return 0;
}

static int stack_write_u64(uint8_t* stack_pages[], uint64_t mapped_bottom_vaddr,
                           uint64_t stack_top_vaddr, uint64_t user_addr, uint64_t value) {
    return stack_write_bytes(stack_pages, mapped_bottom_vaddr, stack_top_vaddr,
                             user_addr, &value, (int)sizeof(value));
}

static int stack_push_u64(uint8_t* stack_pages[], uint64_t mapped_bottom_vaddr,
                          uint64_t stack_top_vaddr, uint64_t* current_sp, uint64_t value) {
    *current_sp -= 8;
    return stack_write_u64(stack_pages, mapped_bottom_vaddr, stack_top_vaddr, *current_sp, value);
}

static int copy_user_string_to_stack(uint8_t* stack_pages[], uint64_t mapped_bottom_vaddr,
                                     uint64_t stack_top_vaddr, uint64_t* current_sp,
                                     const char* src, uint64_t* user_addr_out) {
    if ((uint64_t)src < 0x1000ULL) {
        puts("Exec: invalid user string pointer ");
        puthex((uint64_t)src);
        puts("\r\n");
        return -1;
    }
    int len = 0;
    while (src[len]) len++;
    len++;
    if (*current_sp < mapped_bottom_vaddr + (uint64_t)len) return -1;
    *current_sp -= (uint64_t)len;
    if (*current_sp < mapped_bottom_vaddr || *current_sp >= stack_top_vaddr) return -1;
    if (stack_write_bytes(stack_pages, mapped_bottom_vaddr, stack_top_vaddr, *current_sp, src, len) < 0) {
        return -1;
    }
    *user_addr_out = *current_sp;
    return 0;
}

static int stack_push_auxv_pair(uint8_t* stack_pages[], uint64_t mapped_bottom_vaddr,
                                uint64_t stack_top_vaddr, uint64_t* current_sp,
                                uint64_t type, uint64_t value) {
    return stack_push_u64(stack_pages, mapped_bottom_vaddr, stack_top_vaddr, current_sp, value) < 0 ||
           stack_push_u64(stack_pages, mapped_bottom_vaddr, stack_top_vaddr, current_sp, type) < 0
        ? -1
        : 0;
}

static int stack_push_u64_array_reverse(uint8_t* stack_pages[], uint64_t mapped_bottom_vaddr,
                                        uint64_t stack_top_vaddr, uint64_t* current_sp,
                                        const uint64_t values[], int count) {
    for (int i = count - 1; i >= 0; i--) {
        if (stack_push_u64(stack_pages, mapped_bottom_vaddr, stack_top_vaddr,
                           current_sp, values[i]) < 0) {
            return -1;
        }
    }
    return 0;
}

enum {
    AT_NULL = 0,
    AT_PHDR = 3,
    AT_PHENT = 4,
    AT_PHNUM = 5,
    AT_PAGESZ = 6,
    AT_ENTRY = 9,
    AT_UID = 11,
    AT_EUID = 12,
    AT_GID = 13,
    AT_EGID = 14,
    AT_SECURE = 23,
    AT_RANDOM = 25,
};

int task_prepare_initial_user_stack(arch_address_space_t address_space, struct task* t, const struct elf_info* info, char* const argv[], char* const envp[]) {
    struct arch_task_user_state state;
    uint8_t* stack_pages[USER_STACK_PAGES];
    for (int i = 0; i < USER_STACK_PAGES; i++) stack_pages[i] = 0;
    if (alloc_user_stack(address_space, t, USER_STACK_PAGES, stack_pages) < 0) {
        puts("Exec: alloc_user_stack failed\r\n");
        return -1;
    }

    int argc = 0; if (argv) while (argv[argc]) argc++;
    int envc = 0; if (envp) while (envp[envc]) envc++;
    uint64_t env_ptrs[32]; uint64_t arg_ptrs[32];
    if (argc > 32) argc = 32; if (envc > 32) envc = 32;
    uint64_t current_str_addr = t->user_stack_top;
    current_str_addr -= 16;
    uint64_t random_base = current_str_addr;
    if (stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top,
                        random_base, 0x0123456789abcdefULL) < 0 ||
        stack_write_u64(stack_pages, t->user_stack_bottom, t->user_stack_top,
                        random_base + 8, 0xfedcba9876543210ULL) < 0) {
        return -1;
    }

    for (int i = envc - 1; i >= 0; i--) {
        if (copy_user_string_to_stack(stack_pages, t->user_stack_bottom, t->user_stack_top,
                                      &current_str_addr, envp[i], &env_ptrs[i]) < 0) {
            return -1;
        }
    }
    for (int i = argc - 1; i >= 0; i--) {
        if (copy_user_string_to_stack(stack_pages, t->user_stack_bottom, t->user_stack_top,
                                      &current_str_addr, argv[i], &arg_ptrs[i]) < 0) {
            return -1;
        }
    }

    current_str_addr &= ~7ULL;
    if (current_str_addr < t->user_stack_bottom + (uint64_t)(envc + argc + 21) * 8ULL) {
        return -1;
    }

    if (stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_NULL, 0) < 0 ||
        stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_RANDOM, random_base) < 0 ||
        stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_SECURE, 0) < 0 ||
        stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_EGID, 0) < 0 ||
        stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_GID, 0) < 0 ||
        stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_EUID, 0) < 0 ||
        stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_UID, 0) < 0 ||
        stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_ENTRY, (uint64_t)info->entry) < 0 ||
        stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_PAGESZ, PAGE_SIZE) < 0 ||
        stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_PHNUM, info->phnum) < 0 ||
        stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_PHENT, info->phent) < 0 ||
        stack_push_auxv_pair(stack_pages, t->user_stack_bottom, t->user_stack_top,
                             &current_str_addr, AT_PHDR, info->phdr_vaddr) < 0) return -1;

    // NULL sentinel for envp must appear before auxv in the user-visible stack layout.
    if (stack_push_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, &current_str_addr, 0) < 0) return -1;

    // envp
    if (stack_push_u64_array_reverse(stack_pages, t->user_stack_bottom, t->user_stack_top,
                                     &current_str_addr, env_ptrs, envc) < 0) return -1;
    state.arg2 = current_str_addr;

    // NULL sentinel for argv
    if (stack_push_u64(stack_pages, t->user_stack_bottom, t->user_stack_top, &current_str_addr, 0) < 0) return -1;
    // argv
    if (stack_push_u64_array_reverse(stack_pages, t->user_stack_bottom, t->user_stack_top,
                                     &current_str_addr, arg_ptrs, argc) < 0) return -1;
    state.arg1 = current_str_addr;

    // argc
    if (stack_push_u64(stack_pages, t->user_stack_bottom, t->user_stack_top,
                       &current_str_addr, (uint64_t)argc) < 0) return -1;
    state.arg0 = (uint64_t)argc;
    state.user_sp = current_str_addr;
    task_set_user_state(t, &state);
    return 0;
}

int task_execve(arch_task_exec_frame_t* frame, const char* path, char* const argv[], char* const envp[]) {
    void* file_addr = NULL;
    size_t file_size = 0;
    void* exec_copy_phys = 0;
    struct exec_copy_buf* exec_copy = 0;
    struct elf_load_result load_result;
    uint64_t old_cr3 = 0;
    extern int fs_get_file_data(const char* path, void** data, size_t* size);
    extern int sys_close(int fd);
    exec_copy_phys = pmm_alloc(EXEC_COPY_PAGES);
    if (!exec_copy_phys) {
        return -1;
    }
    exec_copy = (struct exec_copy_buf*)PHYS_TO_VIRT(exec_copy_phys);
    kernel_memset(exec_copy, 0, EXEC_COPY_PAGES * PAGE_SIZE);
    if (copy_user_cstring(path, exec_copy->path, EXEC_MAX_PATH_LEN) < 0 ||
        copy_user_string_vector(argv, exec_copy->argv, exec_copy->argv_storage) < 0 ||
        copy_user_string_vector(envp, exec_copy->envp, exec_copy->envp_storage) < 0) {
        pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
        return -1;
    }
    if (fs_get_file_data(exec_copy->path, &file_addr, &file_size) < 0) {
        puts("Exec: File not found: "); puts(exec_copy->path); puts("\r\n");
        pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
        return -1;
    }
    struct task* t = get_current_task_raw();
    load_result = elf_load_new_user_address_space(file_addr);
    if (!load_result.address_space || !load_result.info.entry) {
        pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
        return -1;
    }
#ifdef __riscv
    puts("  task_execve: stack prep\r\n");
#endif

    if (task_prepare_initial_user_stack(load_result.address_space, t, &load_result.info,
                                        exec_copy->argv, exec_copy->envp) < 0) {
        pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
        arch_vm_destroy_user_address_space(load_result.address_space);
        return -1;
    }

    old_cr3 = arch_task_context_get_address_space(&t->ctx);
#ifdef __riscv
    puts("  task_execve: install image\r\n");
#endif
    task_install_exec_image(t, load_result.address_space, &load_result.info);
    for (int i = 3; i < MAX_FDS; i++) {
        if (t->fds[i].in_use && (t->fds[i].fd_flags & 1)) {
            sys_close(i);
        }
    }
    struct arch_task_user_state state = task_user_state(t);
#ifdef __riscv
    puts("  task_execve: commit\r\n");
#endif
    arch_task_commit_execve(&t->ctx, frame, &state, t->user_fs_base);
#ifdef __riscv
    puts("  task_execve: return\r\n");
#endif
    if (old_cr3 &&
        old_cr3 != arch_task_context_get_address_space(&t->ctx) &&
        old_cr3 != arch_vm_kernel_address_space()) {
        arch_vm_destroy_user_address_space(old_cr3);
    }
    pmm_free(exec_copy_phys, EXEC_COPY_PAGES);
    return 0;
}


void task_main(void) {
    struct task* t = get_current_task();
    struct arch_task_user_state state = task_user_state(t);
    arch_task_sync_user_state(&t->ctx, &state);
    arch_task_enter_initial_user(&state, &t->ctx, &t->os_stack_ptr);
    (void)task_mark_zombie(t, 0);
    while(1) schedule();
}

void task_init(void) {
    // タスク構造体のサイズに合わせて必要なページを確保
    struct task* t = alloc_task_struct();
    struct task* idle = task_create_idle(0);
    if (!t) return;
    if (!idle) return;
    spinlock_init(&g_task_lock);
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    t->pid = next_pid++;
    t->pgid = t->pid;
    t->sid = t->pid;
    t->state = TASK_RUNNING;
    t->cpu_affinity = default_task_cpu_affinity();
    arch_task_context_set_address_space(&t->ctx, arch_vm_kernel_address_space());
    uint64_t sp = arch_task_read_current_stack_pointer();
    t->kstack_top = (sp & ~(PAGE_SIZE - 1)) + PAGE_SIZE;
    t->os_stack_ptr = t->kstack_top;
    t->mmap_end = 0x4000000000ULL;
    t->user_fs_base = 0;
    t->timeslice_ticks = TASK_TIMESLICE_TICKS;
    kernel_strcpy(t->cwd, "/", sizeof(t->cwd));
    struct cpu_local* cpu = &g_cpu_locals[0];
    init_cpu_local(cpu, 0, t, idle, t->kstack_top);
    task_list = t;
    spin_unlock_irqrestore(&g_task_lock, flags);
    init_console_fds(t);
    task_install_cpu_local(0);
    puts("Task system initialized.\r\n");
}

int task_set_affinity(struct task* t, uint32_t cpu_id) {
    int ret;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    ret = task_set_affinity_locked(t, cpu_id);
    spin_unlock_irqrestore(&g_task_lock, flags);
    return ret;
}

int task_mark_ready_on_cpu(struct task* t, uint32_t cpu_id) {
    int ret;
    uint32_t target_cpu;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    target_cpu = (uint32_t)normalize_cpu_affinity(cpu_id);
    ret = task_mark_ready_on_cpu_locked(t, target_cpu);
    if (ret >= 0 && t) {
        task_rebalance_ready_task_locked(t);
        target_cpu = (uint32_t)normalize_cpu_affinity((uint32_t)t->cpu_affinity);
    }
    spin_unlock_irqrestore(&g_task_lock, flags);
    if (ret >= 0) {
        task_request_resched_cpu(target_cpu);
    }
    return ret;
}

int task_mark_sleeping(struct task* t) {
    int ret;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    ret = task_mark_sleeping_locked(t);
    spin_unlock_irqrestore(&g_task_lock, flags);
    return ret;
}

int task_mark_zombie(struct task* t, int exit_status) {
    int ret;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    ret = task_mark_zombie_locked(t, exit_status);
    spin_unlock_irqrestore(&g_task_lock, flags);
    return ret;
}

int task_wake(struct task* t) {
    int ret;
    uint32_t target_cpu = 0;
    if (!t) return -1;
    {
        uint64_t flags = spin_lock_irqsave(&g_task_lock);
        ret = task_wake_locked(t);
        if (ret >= 0) {
            task_rebalance_ready_task_locked(t);
            target_cpu = (uint32_t)normalize_cpu_affinity((uint32_t)t->cpu_affinity);
        }
        spin_unlock_irqrestore(&g_task_lock, flags);
    }
    if (ret < 0) return ret;
    task_request_resched_cpu(target_cpu);
    return 0;
}

int task_reap(struct task* t) {
    int ret;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    ret = task_reap_locked(t);
    spin_unlock_irqrestore(&g_task_lock, flags);
    return ret;
}

struct task* task_create_on_cpu(uint64_t entry, uint64_t user_rsp, uint32_t cpu_id) {
    struct task* t = alloc_task_struct();
    if (!t) return NULL;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    struct arch_task_user_state state;
    t->pid = next_pid++;
    t->pgid = t->pid;
    t->sid = t->pid;
    task_mark_ready_on_cpu_locked(t, cpu_id);
    state.entry_pc = entry;
    state.user_sp = user_rsp;
    state.arg0 = 0;
    state.arg1 = 0;
    state.arg2 = 0;
    task_set_user_state(t, &state);
    task_init_user_layout(t);
    t->mmap_end = 0x4000000000ULL;
    t->user_fs_base = 0;
    t->timeslice_ticks = TASK_TIMESLICE_TICKS;
    kernel_strcpy(t->cwd, "/", sizeof(t->cwd));
    init_console_fds(t);
    void* kstack_phys = pmm_alloc(4);
    t->kstack_top = (uint64_t)PHYS_TO_VIRT(kstack_phys) + 4 * PAGE_SIZE;
    t->os_stack_ptr = t->kstack_top;
    arch_task_context_set_address_space(&t->ctx, arch_vm_create_user_address_space());
    t->os_stack_ptr = arch_task_prepare_kernel_entry(
        &t->ctx,
        t->kstack_top,
        (uint64_t)task_main,
        arch_task_context_get_address_space(&t->ctx));
    t->next = task_list;
    task_list = t;
    spin_unlock_irqrestore(&g_task_lock, flags);
    return t;
}

struct task* task_create(uint64_t entry, uint64_t user_rsp) {
    return task_create_on_cpu(entry, user_rsp, (uint32_t)default_task_cpu_affinity());
}

struct task* task_create_idle(uint32_t cpu_id) {
    struct task* t = alloc_task_struct();
    if (!t) return NULL;
    t->pid = 0;
    t->state = TASK_RUNNING;
    t->cpu_affinity = (int)cpu_id;
    arch_task_context_set_address_space(&t->ctx, arch_vm_kernel_address_space());
    t->mmap_end = 0x4000000000ULL;
    t->timeslice_ticks = TASK_TIMESLICE_TICKS;
    kernel_strcpy(t->cwd, "/", sizeof(t->cwd));
    void* kstack_phys = pmm_alloc(4);
    if (!kstack_phys) return NULL;
    t->kstack_top = (uint64_t)PHYS_TO_VIRT(kstack_phys) + 4 * PAGE_SIZE;
    t->os_stack_ptr = t->kstack_top;
    task_init_idle_context(t);
    arch_task_context_init_fp_state(&t->ctx);
    (void)cpu_id;
    return t;
}

int task_fork(arch_task_exec_frame_t* frame) {
    struct task* parent = get_current_task_raw();
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    struct task* child = alloc_task_struct();
    if (!child) {
        spin_unlock_irqrestore(&g_task_lock, flags);
        return -1;
    }
    child->pid = next_pid++;
    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    uint32_t spawn_cpu = g_fork_spread_enabled
        ? choose_spawn_cpu_locked((uint32_t)parent->cpu_affinity)
        : (uint32_t)normalize_cpu_affinity((uint32_t)parent->cpu_affinity);
    task_mark_ready_on_cpu_locked(child, spawn_cpu);
    child->heap_break = parent->heap_break;
    child->mmap_end = parent->mmap_end;
    task_copy_user_state(child, parent);
    task_copy_user_layout(child, parent);
    child->user_fs_base = parent->user_fs_base;
    child->timeslice_ticks = TASK_TIMESLICE_TICKS;
    arch_task_context_set_address_space(
        &child->ctx,
        arch_vm_clone_address_space(arch_task_context_get_address_space(&parent->ctx)));
    kernel_strcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    void* kstack_phys = pmm_alloc(4);
    child->kstack_top = (uint64_t)PHYS_TO_VIRT(kstack_phys) + 4 * PAGE_SIZE;
    child->os_stack_ptr = child->kstack_top;
    arch_task_commit_fork_child(&child->ctx, child->kstack_top, &parent->ctx, frame);
    for (int i = 0; i < MAX_FDS; i++) {
        child->fds[i] = parent->fds[i];
        if (child->fds[i].in_use && child->fds[i].type == FT_PIPE) {
            pipe_t* pipe = (pipe_t*)child->fds[i].data;
            if (pipe) {
                uint64_t pipe_flags = spin_lock_irqsave(&pipe->lock);
                pipe->ref_count++;
                spin_unlock_irqrestore(&pipe->lock, pipe_flags);
            }
        } else if (child->fds[i].in_use && child->fds[i].type == FT_SOCKET) {
            net_socket_dup_fd(&child->fds[i]);
        }
    }
    child->next = task_list;
    task_list = child;
    task_rebalance_ready_task_locked(child);
    spin_unlock_irqrestore(&g_task_lock, flags);
    task_request_resched_cpu((uint32_t)child->cpu_affinity);
    return child->pid;
}

void schedule(void) {
    struct cpu_local* cpu = this_cpu();
    struct task* current_task = cpu ? cpu->current_task : NULL;
    struct task* next;
    struct task* prev;
    if (!current_task) return;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    next = task_runq_pop_locked(cpu);
    if (!next) {
        if (current_task->state == TASK_RUNNING) {
            spin_unlock_irqrestore(&g_task_lock, flags);
            return;
        }
        next = cpu->idle_task;
    }
    if (next == current_task) {
        spin_unlock_irqrestore(&g_task_lock, flags);
        return;
    }
    prev = current_task;
    if (prev->state == TASK_RUNNING && !task_is_idle_task(prev)) {
        task_mark_ready_on_cpu_locked(prev, (uint32_t)prev->cpu_affinity);
    }
    cpu->current_task = next;
    next->on_runq = 0;
    next->state = TASK_RUNNING;
    if (next->timeslice_ticks <= 0) next->timeslice_ticks = TASK_TIMESLICE_TICKS;
    cpu->kernel_stack = next->kstack_top;
    arch_task_prepare_schedule_switch(cpu->cpu_id, next->kstack_top, cpu, next->user_fs_base);
    spin_unlock_irqrestore(&g_task_lock, flags);
    arch_context_switch(&next->ctx, &prev->ctx);
}

void task_idle_loop(int poll_network) {
    for (;;) {
        if (poll_network) {
            net_poll();
        }
        arch_task_idle_wait_once();
        if (task_consume_resched()) {
            kernel_yield();
        }
    }
}
