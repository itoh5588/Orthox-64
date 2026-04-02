#include <stddef.h>
#include <stdint.h>
#include "elf64.h"
#include "pmm.h"
#include "riscv64/boot.h"
#include "riscv64/elf.h"
#include "riscv64/syscall.h"
#include "riscv64/vm.h"
#include "smp.h"
#include "spinlock.h"
#include "syscall.h"
#include "task.h"
#include "vmm.h"

__attribute__((weak)) uint64_t g_hhdm_offset;
static struct smp_cpu_info g_riscv64_stub_bsp_cpu = { 0, 0, 0, 1, 1 };
static const uint8_t g_riscv64_bootstrap_user_code[] = {
    0xb7, 0x02, 0x00, 0x10, /* lui t0, 0x10000 */
    0x13, 0x03, 0x50, 0x05, /* addi t1, zero, 'U' */
    0x23, 0x80, 0x62, 0x00, /* sb t1, 0(t0) */
    0x13, 0x03, 0xf0, 0x04, /* addi t1, zero, 'O' */
    0x23, 0x80, 0x62, 0x00, /* sb t1, 0(t0) */
    0x13, 0x03, 0xb0, 0x04, /* addi t1, zero, 'K' */
    0x23, 0x80, 0x62, 0x00, /* sb t1, 0(t0) */
    0x13, 0x03, 0xa0, 0x00, /* addi t1, zero, '\n' */
    0x23, 0x80, 0x62, 0x00, /* sb t1, 0(t0) */
    0x6f, 0x00, 0x00, 0x00, /* j . */
};

static int riscv64_stub_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void riscv64_stub_copyout(char* dst, const char* src, size_t len) {
    if (!dst || !src) return;
    for (size_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
}

static size_t riscv64_stub_strlen(const char* s) {
    size_t len = 0;
    if (!s) return 0;
    while (s[len] != '\0') len++;
    return len;
}

void kernel_lock_enter(void) {
}

void kernel_lock_exit(void) {
}

void net_poll(void) {
}

__attribute__((weak)) uint64_t arch_time_now_ms(void) {
    return 0;
}

__attribute__((weak)) int kernel_lock_held(void) {
    return 0;
}

__attribute__((weak)) void kernel_yield(void) {
}

__attribute__((weak)) void task_on_timer_tick(void) {
}

void syscall_dispatch(arch_syscall_frame_t* frame) {
    const char* src;
    size_t len;
    if (!frame) return;

    switch (arch_syscall_number(frame)) {
        case SYS_WRITE:
            if ((int)arch_syscall_arg0(frame) != 1 && (int)arch_syscall_arg0(frame) != 2) {
                arch_syscall_set_return(frame, (uint64_t)-1);
                return;
            }
            src = (const char*)(uintptr_t)arch_syscall_arg1(frame);
            len = (size_t)arch_syscall_arg2(frame);
            for (size_t i = 0; i < len; i++) {
                riscv64_uart_putchar(src[i]);
            }
            arch_syscall_set_return(frame, len);
            return;
        case SYS_GETCWD:
            {
                struct task* task = get_current_task();
                char* dst = (char*)(uintptr_t)arch_syscall_arg0(frame);
                size_t dst_size = (size_t)arch_syscall_arg1(frame);
                const char* cwd = (task && task->cwd[0]) ? task->cwd : "/";
                size_t cwd_len = riscv64_stub_strlen(cwd) + 1U;
                if (!dst || dst_size < cwd_len) {
                    arch_syscall_set_return(frame, 0);
                    return;
                }
                riscv64_stub_copyout(dst, cwd, cwd_len);
                arch_syscall_set_return(frame, (uint64_t)(uintptr_t)dst);
                return;
            }
        case SYS_GETPID:
            arch_syscall_set_return(frame, 1);
            return;
        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            riscv64_uart_puts("  bootstrap user exit\n");
            riscv64_wait_forever();
            return;
        default:
            arch_syscall_set_return(frame, (uint64_t)-38);
            return;
    }
}

__attribute__((weak)) void arch_context_switch(struct arch_task_context* next_ctx,
                                               struct arch_task_context* prev_ctx) {
    (void)next_ctx;
    (void)prev_ctx;
}

__attribute__((weak)) struct elf_load_result elf_load_new_user_address_space(void* elf_data) {
    struct elf_load_result result;
    arch_address_space_t user_address_space = 0;
    result.address_space = 0;
    result.info.entry = 0;
    result.info.phdr_vaddr = 0;
    result.info.phent = 0;
    result.info.phnum = 0;
    if (elf_data != (void*)g_riscv64_bootstrap_user_code) return result;
    riscv64_uart_puts("  bootstrap elf: create user aspace\n");
    user_address_space = arch_vm_create_user_address_space();
    if (!user_address_space) return result;
    result.address_space = user_address_space;
    riscv64_uart_puts("  bootstrap elf: map uart\n");
    arch_vm_map_page(result.address_space,
                     RISCV64_QEMU_VIRT_UART0_BASE,
                     RISCV64_QEMU_VIRT_UART0_BASE,
                     RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_U);
    riscv64_uart_puts("  bootstrap elf: map text\n");
    if (riscv64_elf_load_segment_bootstrap(result.address_space,
                                           0x0000000000400000ULL,
                                           g_riscv64_bootstrap_user_code,
                                           sizeof(g_riscv64_bootstrap_user_code),
                                           4096,
                                           arch_vm_user_page_flags(0, 1)) < 0) {
        arch_vm_destroy_user_address_space(result.address_space);
        result.address_space = 0;
        return result;
    }
    riscv64_uart_puts("  bootstrap elf: done\n");
    result.info.entry = (void*)(uintptr_t)0x0000000000400000ULL;
    return result;
}

__attribute__((weak)) int fs_get_file_data(const char* path, void** data, size_t* size) {
    if (!riscv64_stub_streq(path, "/bootstrap-user")) {
        if (data) *data = 0;
        if (size) *size = 0;
        return -1;
    }
    if (data) *data = (void*)g_riscv64_bootstrap_user_code;
    if (size) *size = sizeof(g_riscv64_bootstrap_user_code);
    return 0;
}

__attribute__((weak)) void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((weak)) void* pmm_alloc(size_t pages) {
    uint64_t first = 0;
    uint64_t allocated[16];
    if (pages == 0 || pages > 16) return 0;
    for (size_t i = 0; i < pages; i++) {
        allocated[i] = riscv64_vm_bootstrap_alloc_page();
        if (!allocated[i]) {
            for (size_t j = 0; j < i; j++) riscv64_vm_bootstrap_free_page(allocated[j]);
            return 0;
        }
        if (i == 0) {
            first = allocated[i];
        } else if (allocated[i] != first + i * PAGE_SIZE) {
            for (size_t j = 0; j <= i; j++) riscv64_vm_bootstrap_free_page(allocated[j]);
            return 0;
        }
    }
    return (void*)(uintptr_t)first;
}

__attribute__((weak)) void pmm_free(void* addr, size_t pages) {
    uint64_t base = (uint64_t)(uintptr_t)addr;
    if (!addr || pages == 0) return;
    for (size_t i = 0; i < pages; i++) {
        riscv64_vm_bootstrap_free_page(base + i * PAGE_SIZE);
    }
}

__attribute__((weak)) void puts(const char* s) {
    riscv64_uart_puts(s);
}

__attribute__((weak)) void puthex(uint64_t value) {
    riscv64_uart_puthex64(value);
}

__attribute__((weak)) void net_socket_dup_fd(file_descriptor_t* f) {
    (void)f;
}

__attribute__((weak)) int sys_close(int fd) {
    (void)fd;
    return 0;
}

__attribute__((weak)) const struct smp_cpu_info* smp_get_cpu_info(uint32_t cpu_index) {
    if (cpu_index != 0) return 0;
    return &g_riscv64_stub_bsp_cpu;
}

__attribute__((weak)) uint32_t smp_get_started_cpu_count(void) {
    return 1;
}

__attribute__((weak)) void smp_send_resched_ipi(uint32_t cpu_id) {
    (void)cpu_id;
}

__attribute__((weak)) uint64_t irq_save_disable(void) {
    return 0;
}

__attribute__((weak)) void irq_restore(uint64_t flags) {
    (void)flags;
}

__attribute__((weak)) void spinlock_init(spinlock_t* lock) {
    if (!lock) return;
    lock->locked = 0;
}

__attribute__((weak)) void spin_lock(spinlock_t* lock) {
    (void)lock;
}

__attribute__((weak)) void spin_unlock(spinlock_t* lock) {
    (void)lock;
}

__attribute__((weak)) uint64_t spin_lock_irqsave(spinlock_t* lock) {
    (void)lock;
    return 0;
}

__attribute__((weak)) void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags) {
    (void)lock;
    (void)flags;
}
