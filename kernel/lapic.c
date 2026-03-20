#include <stdint.h>
#include "lapic.h"
#include "vmm.h"
#include "idt.h"

static volatile uint32_t* lapic_base;
static volatile uint64_t g_ticks_ms = 0;
static uint32_t g_timer_ticks_per_interval = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %b0, %w1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %w1, %b0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static void lapic_write(uint32_t reg, uint32_t data) {
    lapic_base[reg >> 2] = data;
}

static uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg >> 2];
}

void lapic_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}

void lapic_timer_tick(void) {
    g_ticks_ms += SCHED_TICK_MS;
}

uint64_t lapic_get_ticks_ms(void) {
    return g_ticks_ms;
}

// PIT (Programmable Interval Timer) を使用した一時的な待機
static void pit_prepare_sleep(uint16_t count) {
    outb(0x43, 0x34);
    outb(0x40, count & 0xFF);
    outb(0x40, (count >> 8) & 0xFF);
}

static void pit_wait(void) {
    while (1) {
        outb(0x43, 0xE2);
        if (inb(0x40) & 0x80) break;
    }
}

void lapic_init(void) {
    lapic_base = (uint32_t*)PHYS_TO_VIRT(LAPIC_BASE_ADDR);

    // Spurious Interrupt Vector Register の設定
    lapic_write(LAPIC_REG_SVR, lapic_read(LAPIC_REG_SVR) | 0x1FF);

    // タイマーのキャリブレーション (PIT を使用して 10ms の LAPIC ticks を測定)
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    pit_prepare_sleep(PIT_COUNT_FOR_TICK);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);
    
    pit_wait();
    
    g_timer_ticks_per_interval = 0xFFFFFFFF - lapic_read(LAPIC_REG_TIMER_CUR);

    // 定期的な割り込みの設定
    lapic_write(LAPIC_REG_LVT_TIMER, INT_VECTOR_TIMER | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_REG_TIMER_INIT, g_timer_ticks_per_interval);
}

void lapic_init_cpu(void) {
    if (!lapic_base) {
        lapic_base = (uint32_t*)PHYS_TO_VIRT(LAPIC_BASE_ADDR);
    }

    lapic_write(LAPIC_REG_SVR, lapic_read(LAPIC_REG_SVR) | 0x1FF);
    lapic_write(LAPIC_REG_LVT_TIMER, INT_VECTOR_TIMER | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    if (g_timer_ticks_per_interval) {
        lapic_write(LAPIC_REG_TIMER_INIT, g_timer_ticks_per_interval);
    }
}
