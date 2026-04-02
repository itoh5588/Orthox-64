#include <stdint.h>
#include "riscv64/boot.h"
#include "riscv64/csr.h"
#include "riscv64/time.h"
#include "smp.h"
#include "spinlock.h"
#include "task.h"

uint64_t g_hhdm_offset;

static spinlock_t g_riscv64_kernel_lock;
static struct smp_cpu_info g_riscv64_bsp_cpu = { 0, 0, 0, 1, 1 };

void net_poll(void) {
}

uint64_t arch_time_now_ms(void) {
    /*
     * QEMU virt/OpenSBI exposes the platform timer at 10 MHz.
     * Bootstrap and smoke only need a monotonic millisecond clock.
     */
    return riscv64_read_time() / 10000ULL;
}

uint64_t arch_time_cpu_ms(uint32_t cpu_id) {
    (void)cpu_id;
    return arch_time_now_ms();
}

uint64_t irq_save_disable(void) {
    uint64_t flags = riscv64_read_sstatus();
    riscv64_write_sstatus(flags & ~RISCV64_SSTATUS_SIE);
    return flags;
}

void irq_restore(uint64_t flags) {
    uint64_t sstatus = riscv64_read_sstatus();
    sstatus &= ~RISCV64_SSTATUS_SIE;
    sstatus |= (flags & RISCV64_SSTATUS_SIE);
    riscv64_write_sstatus(sstatus);
}

void spinlock_init(spinlock_t* lock) {
    if (!lock) return;
    lock->locked = 0;
}

void spin_lock(spinlock_t* lock) {
    if (!lock) return;
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("nop");
    }
}

void spin_unlock(spinlock_t* lock) {
    if (!lock) return;
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

uint64_t spin_lock_irqsave(spinlock_t* lock) {
    uint64_t flags = irq_save_disable();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags) {
    spin_unlock(lock);
    irq_restore(flags);
}

void kernel_lock_enter(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) {
        spin_lock(&g_riscv64_kernel_lock);
        return;
    }
    if (cpu->kernel_lock_depth++ == 0) {
        spin_lock(&g_riscv64_kernel_lock);
    }
}

void kernel_lock_exit(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) {
        spin_unlock(&g_riscv64_kernel_lock);
        return;
    }
    if (cpu->kernel_lock_depth == 0) return;
    cpu->kernel_lock_depth--;
    if (cpu->kernel_lock_depth == 0) {
        spin_unlock(&g_riscv64_kernel_lock);
    }
}

int kernel_lock_held(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) {
        return g_riscv64_kernel_lock.locked != 0;
    }
    return cpu->kernel_lock_depth != 0;
}

void kernel_yield(void) {
    struct cpu_local* cpu = get_cpu_local();
    uint32_t depth = cpu ? cpu->kernel_lock_depth : 0;
    for (uint32_t i = 0; i < depth; i++) kernel_lock_exit();
    schedule();
    for (uint32_t i = 0; i < depth; i++) kernel_lock_enter();
}

void puts(const char* s) {
    riscv64_uart_puts(s);
}

void puthex(uint64_t value) {
    riscv64_uart_puthex64(value);
}

const struct smp_cpu_info* smp_get_cpu_info(uint32_t cpu_index) {
    if (cpu_index != 0) return 0;
    return &g_riscv64_bsp_cpu;
}

uint32_t smp_get_started_cpu_count(void) {
    return 1;
}

void smp_send_resched_ipi(uint32_t cpu_id) {
    (void)cpu_id;
}
