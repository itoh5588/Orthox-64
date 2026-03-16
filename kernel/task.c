#include <stdint.h>
#include <stddef.h>
#include "task.h"
#include "pmm.h"
#include "vmm.h"
#include "gdt.h"
#include "limine.h"
#include "elf64.h"

struct cpu_data {
    uint64_t kernel_stack;
    uint64_t user_stack;
};

static struct cpu_data g_cpu_data;
struct task* current_task = NULL;
struct task* task_list = NULL;
static int next_pid = 1;
static volatile int g_resched_pending = 0;

#define TASK_TIMESLICE_TICKS 5

#define USER_STACK_TOP_VADDR   0x7FFFFFFFF000ULL
#define USER_STACK_PAGES       256
#define USER_STACK_GUARD_PAGES 1

extern volatile struct limine_module_request module_request;

#define MSR_FS_BASE        0xC0000100
#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = val >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

extern void switch_context(struct task_context* next_ctx, struct task_context* prev_ctx);
extern void puts(const char* s);
extern void puthex(uint64_t v);
extern void fork_child_entry(void);

struct task* get_current_task(void) {
    return current_task;
}

void task_request_resched(void) {
    g_resched_pending = 1;
}

int task_consume_resched(void) {
    if (!g_resched_pending) return 0;
    g_resched_pending = 0;
    return 1;
}

void task_on_timer_tick(void) {
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

static int alloc_user_stack(uint64_t* pml4_virt, struct task* t, int stack_pages, uint8_t** stack_mem_out) {
    if (!pml4_virt || !t || stack_pages <= USER_STACK_GUARD_PAGES) return -1;
    void* stack_phys = pmm_alloc(stack_pages - USER_STACK_GUARD_PAGES);
    if (!stack_phys) return -1;
    uint64_t stack_bottom_vaddr = USER_STACK_TOP_VADDR - (uint64_t)stack_pages * PAGE_SIZE;
    uint64_t mapped_bottom_vaddr = stack_bottom_vaddr + USER_STACK_GUARD_PAGES * PAGE_SIZE;
    for (int i = 0; i < stack_pages - USER_STACK_GUARD_PAGES; i++) {
        vmm_map_page(pml4_virt,
                     mapped_bottom_vaddr + (uint64_t)i * PAGE_SIZE,
                     (uint64_t)stack_phys + (uint64_t)i * PAGE_SIZE,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }
    t->user_stack_top = USER_STACK_TOP_VADDR;
    t->user_stack_bottom = mapped_bottom_vaddr;
    t->user_stack_guard = stack_bottom_vaddr;
    if (stack_mem_out) *stack_mem_out = (uint8_t*)PHYS_TO_VIRT(stack_phys);
    return 0;
}

static int copy_user_string_to_stack(uint8_t* stack_mem, uint64_t mapped_bottom_vaddr,
                                     uint64_t stack_top_vaddr, uint64_t* current_sp,
                                     const char* src, uint64_t* user_addr_out) {
    int len = 0;
    while (src[len]) len++;
    len++;
    if (*current_sp < mapped_bottom_vaddr + (uint64_t)len) return -1;
    *current_sp -= (uint64_t)len;
    uint64_t offset = *current_sp - mapped_bottom_vaddr;
    if (*current_sp < mapped_bottom_vaddr || *current_sp >= stack_top_vaddr) return -1;
    for (int j = 0; j < len; j++) stack_mem[offset + (uint64_t)j] = (uint8_t)src[j];
    *user_addr_out = *current_sp;
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

struct syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

int task_prepare_initial_user_stack(uint64_t* pml4_virt, struct task* t, const struct elf_info* info, char* const argv[], char* const envp[]) {
    uint8_t* stack_mem = 0;
    if (alloc_user_stack(pml4_virt, t, USER_STACK_PAGES, &stack_mem) < 0) {
        return -1;
    }

    int argc = 0; if (argv) while (argv[argc]) argc++;
    int envc = 0; if (envp) while (envp[envc]) envc++;
    uint64_t env_ptrs[32]; uint64_t arg_ptrs[32];
    if (argc > 32) argc = 32; if (envc > 32) envc = 32;

    uint64_t current_str_addr = t->user_stack_top;
    current_str_addr -= 16;
    uint64_t random_base = current_str_addr;
    *(uint64_t*)(stack_mem + (random_base - t->user_stack_bottom)) = 0x0123456789abcdefULL;
    *(uint64_t*)(stack_mem + (random_base - t->user_stack_bottom) + 8) = 0xfedcba9876543210ULL;

    for (int i = envc - 1; i >= 0; i--) {
        if (copy_user_string_to_stack(stack_mem, t->user_stack_bottom, t->user_stack_top,
                                      &current_str_addr, envp[i], &env_ptrs[i]) < 0) {
            return -1;
        }
    }
    for (int i = argc - 1; i >= 0; i--) {
        if (copy_user_string_to_stack(stack_mem, t->user_stack_bottom, t->user_stack_top,
                                      &current_str_addr, argv[i], &arg_ptrs[i]) < 0) {
            return -1;
        }
    }

    current_str_addr &= ~7ULL;
    if (current_str_addr < t->user_stack_bottom + (uint64_t)(envc + argc + 21) * 8ULL) {
        return -1;
    }

    // NULL sentinel for envp
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = 0;

    // auxv (push value first, then type)
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = 0;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_NULL;

    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = random_base;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_RANDOM;

    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = 0;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_SECURE;

    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = 0;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_EGID;

    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = 0;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_GID;

    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = 0;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_EUID;

    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = 0;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_UID;

    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = (uint64_t)info->entry;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_ENTRY;

    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = PAGE_SIZE;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_PAGESZ;

    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = info->phnum;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_PHNUM;

    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = info->phent;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_PHENT;

    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = info->phdr_vaddr;
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = AT_PHDR;

    // envp
    for (int i = envc - 1; i >= 0; i--) {
        current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = env_ptrs[i];
    }
    t->user_envp = current_str_addr;

    // NULL sentinel for argv
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = 0;
    // argv
    for (int i = argc - 1; i >= 0; i--) {
        current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = arg_ptrs[i];
    }
    t->user_argv = current_str_addr;

    // argc
    current_str_addr -= 8; *(uint64_t*)(stack_mem + (current_str_addr - t->user_stack_bottom)) = (uint64_t)argc;
    t->user_argc = (uint64_t)argc;

    t->user_stack = current_str_addr;
    return 0;
}

int task_execve(struct syscall_frame* frame, const char* path, char* const argv[], char* const envp[]) {
    void* file_addr = NULL;
    size_t file_size = 0;
    extern int fs_get_file_data(const char* path, void** data, size_t* size);
    extern int sys_close(int fd);
    if (fs_get_file_data(path, &file_addr, &file_size) < 0) {
        puts("Exec: File not found: "); puts(path); puts("\r\n");
        return -1;
    }
    struct task* t = current_task;
    void* pml4_phys = pmm_alloc(1);
    uint64_t* pml4_virt = (uint64_t*)PHYS_TO_VIRT(pml4_phys);
    uint64_t* kernel_pml4 = vmm_get_kernel_pml4();
    for (int i = 0; i < 512; i++) {
        pml4_virt[i] = (i >= 256) ? kernel_pml4[i] : 0;
    }
    struct elf_info info = elf_load(pml4_virt, file_addr);
    if (!info.entry) {
        puts("Exec: ELF load failed\r\n");
        return -1;
    }

    if (task_prepare_initial_user_stack(pml4_virt, t, &info, argv, envp) < 0) {
        puts("Exec: stack preparation failed\r\n");
        return -1;
    }

    t->ctx.cr3 = (uint64_t)pml4_phys;
    t->heap_break = info.max_vaddr;
    t->mmap_end = 0x4000000000ULL;
    t->user_entry = (uint64_t)info.entry;
    t->user_fs_base = 0;
    for (int i = 3; i < MAX_FDS; i++) {
        if (t->fds[i].in_use && (t->fds[i].fd_flags & 1)) {
            sys_close(i);
        }
    }
    frame->r15 = 0;
    frame->r14 = 0;
    frame->r13 = 0;
    frame->r12 = 0;
    frame->rbp = 0;
    frame->rbx = 0;
    frame->r9 = 0;
    frame->r8 = 0;
    frame->r10 = 0;
    frame->rax = 0;
    frame->rip = t->user_entry;
    frame->rsp = t->user_stack;
    frame->rdi = t->user_argc;
    frame->rsi = t->user_argv;
    frame->rdx = t->user_envp;
    __asm__ volatile("mov %0, %%cr3" : : "r"(t->ctx.cr3) : "memory");
    wrmsr(MSR_FS_BASE, t->user_fs_base);
    return 0;
}


void task_main(void) {
    struct task* t = get_current_task();
    extern int call_app(int argc, char** argv, uint16_t ss, uint64_t rip, uint64_t rsp, uint64_t* os_stack_ptr);
    call_app(0, NULL, 0x1B, t->user_entry, t->user_stack, &t->os_stack_ptr);
    t->state = TASK_ZOMBIE;
    while(1) schedule();
}

void task_init(void) {
    // タスク構造体のサイズに合わせて必要なページを確保
    int task_pages = (sizeof(struct task) + PAGE_SIZE - 1) / PAGE_SIZE;
    void* phys = pmm_alloc(task_pages);
    struct task* t = (struct task*)PHYS_TO_VIRT(phys);
    kernel_memset(t, 0, sizeof(struct task));
    t->pid = next_pid++;
    t->pgid = t->pid;
    t->sid = t->pid;
    t->state = TASK_RUNNING;
    t->ctx.cr3 = vmm_get_kernel_pml4_phys();
    uint64_t sp; __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    t->kstack_top = (sp & ~(PAGE_SIZE - 1)) + PAGE_SIZE; 
    t->os_stack_ptr = t->kstack_top;
    t->mmap_end = 0x4000000000ULL;
    t->user_fs_base = 0;
    t->timeslice_ticks = TASK_TIMESLICE_TICKS;
    kernel_strcpy(t->cwd, "/", sizeof(t->cwd));
    current_task = t;
    task_list = t;
    init_console_fds(t);
    g_cpu_data.kernel_stack = t->kstack_top;
    wrmsr(MSR_GS_BASE, (uintptr_t)&g_cpu_data);
    wrmsr(MSR_KERNEL_GS_BASE, (uintptr_t)&g_cpu_data);
    puts("Task system initialized.\r\n");
}

struct task* task_create(uint64_t entry, uint64_t user_rsp) {
    int task_pages = (sizeof(struct task) + PAGE_SIZE - 1) / PAGE_SIZE;
    void* t_phys = pmm_alloc(task_pages);
    struct task* t = (struct task*)PHYS_TO_VIRT(t_phys);
    kernel_memset(t, 0, sizeof(struct task));
    t->pid = next_pid++;
    t->pgid = t->pid;
    t->sid = t->pid;
    t->state = TASK_READY;
    t->user_entry = entry;
    t->user_stack = user_rsp;
    t->user_stack_top = USER_STACK_TOP_VADDR;
    t->user_stack_bottom = 0;
    t->user_stack_guard = 0;
    t->mmap_end = 0x4000000000ULL;
    t->user_fs_base = 0;
    t->timeslice_ticks = TASK_TIMESLICE_TICKS;
    kernel_strcpy(t->cwd, "/", sizeof(t->cwd));
    init_console_fds(t);
    void* kstack_phys = pmm_alloc(4);
    t->kstack_top = (uint64_t)PHYS_TO_VIRT(kstack_phys) + 4 * PAGE_SIZE;
    t->os_stack_ptr = t->kstack_top;
    void* pml4_phys = pmm_alloc(1);
    uint64_t* pml4_virt = (uint64_t*)PHYS_TO_VIRT(pml4_phys);
    uint64_t* kernel_pml4 = vmm_get_kernel_pml4();
    for (int i = 0; i < 512; i++) {
        pml4_virt[i] = (i >= 256) ? kernel_pml4[i] : 0;
    }
    t->ctx.cr3 = (uint64_t)pml4_phys;
    t->ctx.rip = (uint64_t)task_main;
    uint64_t* sp = (uint64_t*)(t->kstack_top - 8);
    *sp = t->ctx.rip;
    *(--sp) = 0x202;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    t->ctx.rsp = (uint64_t)sp;
    t->ctx.cs = 0x08;
    t->ctx.ss = 0x10;
    t->ctx.fxsave_area[0] = 0x7f; t->ctx.fxsave_area[1] = 0x03;
    t->ctx.fxsave_area[24] = 0x80; t->ctx.fxsave_area[25] = 0x1f;
    t->next = task_list;
    task_list = t;
    return t;
}

int task_fork(struct syscall_frame* frame) {
    struct task* parent = current_task;
    int task_pages = (sizeof(struct task) + PAGE_SIZE - 1) / PAGE_SIZE;
    void* child_phys = pmm_alloc(task_pages);
    struct task* child = (struct task*)PHYS_TO_VIRT(child_phys);
    kernel_memset(child, 0, sizeof(struct task));
    child->pid = next_pid++;
    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->state = TASK_READY;
    child->heap_break = parent->heap_break;
    child->mmap_end = parent->mmap_end;
    child->user_entry = parent->user_entry;
    child->user_stack = parent->user_stack;
    child->user_stack_top = parent->user_stack_top;
    child->user_stack_bottom = parent->user_stack_bottom;
    child->user_stack_guard = parent->user_stack_guard;
    child->user_fs_base = parent->user_fs_base;
    child->timeslice_ticks = TASK_TIMESLICE_TICKS;
    child->ctx.cr3 = vmm_copy_pml4((uint64_t*)PHYS_TO_VIRT(parent->ctx.cr3));
    kernel_strcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    void* kstack_phys = pmm_alloc(4);
    child->kstack_top = (uint64_t)PHYS_TO_VIRT(kstack_phys) + 4 * PAGE_SIZE;
    child->os_stack_ptr = child->kstack_top;
    struct syscall_frame* child_frame = (struct syscall_frame*)(child->kstack_top - sizeof(struct syscall_frame));
    for (size_t i = 0; i < sizeof(struct syscall_frame); i++) {
        ((uint8_t*)child_frame)[i] = ((uint8_t*)frame)[i];
    }
    child_frame->rax = 0;
    uint64_t* sp = (uint64_t*)child_frame;
    *(--sp) = (uint64_t)fork_child_entry;
    *(--sp) = 0x202;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    child->ctx.rip = (uint64_t)fork_child_entry;
    child->ctx.rsp = (uint64_t)sp;
    child->ctx.rflags = 0x202;
    child->ctx.cs = 0x08;
    child->ctx.ss = 0x10;
    for (int i = 0; i < 512; i++) child->ctx.fxsave_area[i] = parent->ctx.fxsave_area[i];
    for (int i = 0; i < MAX_FDS; i++) {
        child->fds[i] = parent->fds[i];
        if (child->fds[i].in_use && child->fds[i].type == FT_PIPE) {
            pipe_t* pipe = (pipe_t*)child->fds[i].data;
            if (pipe) pipe->ref_count++;
        }
    }
    child->next = task_list;
    task_list = child;
    return child->pid;
}

void schedule(void) {
    if (!current_task) return;
    struct task* next = current_task->next;
    if (!next) next = task_list;
    struct task* start = next;
    while (next->state != TASK_READY && next->state != TASK_RUNNING) {
        next = next->next;
        if (!next) next = task_list;
        if (next == start) return;
    }
    if (next == current_task) return;
    struct task* prev = current_task;
    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    current_task = next;
    next->state = TASK_RUNNING;
    if (next->timeslice_ticks <= 0) next->timeslice_ticks = TASK_TIMESLICE_TICKS;
    tss_set_stack(next->kstack_top);
    g_cpu_data.kernel_stack = next->kstack_top;
    wrmsr(MSR_FS_BASE, next->user_fs_base);
    switch_context(&next->ctx, &prev->ctx);
}
