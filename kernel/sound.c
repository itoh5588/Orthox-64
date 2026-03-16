#include <stdint.h>
#include "sound.h"
#include "pmm.h"
#include "vmm.h"

void puts(const char* s);
void puthex(uint64_t v);

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static int sb16_initialized = 0;
static int sb16_failed = 0;
static uint8_t* sb16_dma_buf = 0;
static uint32_t sb16_dma_phys = 0;
static int sb16_error_reported = 0;

static void sb16_report_once(const char* msg) {
    if (sb16_error_reported) return;
    sb16_error_reported = 1;
    puts("[sb16] ");
    puts(msg);
    puts("\r\n");
}

static int sb16_wait_write_ready(void) {
    // DSP write-buffer status polling (legacy SB16 I/O protocol):
    // bit7=1 means busy, bit7=0 means command/data can be written.
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x22C) & 0x80) == 0) return 0;
    }
    return -1;
}

static int sb16_write(uint8_t v) {
    if (sb16_wait_write_ready() != 0) return -1;
    outb(0x22C, v);
    return 0;
}

static int sb16_prepare_dma_buffer(void) {
    if (sb16_dma_buf) return 0;

    void* phys = pmm_get_isa_dma_page();
    if (!phys) {
        sb16_report_once("no ISA DMA page reserved below 16MiB");
        return -1;
    }

    uint64_t p = (uint64_t)phys;
    sb16_dma_phys = (uint32_t)p;
    sb16_dma_buf = (uint8_t*)PHYS_TO_VIRT(phys);
    if (!sb16_dma_buf) {
        sb16_report_once("failed to map ISA DMA page");
        return -1;
    }

    if ((p & 0xFFFFULL) > (0x10000ULL - 4096ULL)) {
        sb16_report_once("reserved ISA DMA page crosses 64KiB boundary");
        return -1;
    }

    return 0;
}

static void dma_ch1_program(uint32_t phys, uint16_t len) {
    uint16_t count = (uint16_t)(len - 1);

    // mask channel 1
    outb(0x0A, 0x05);
    // clear byte pointer flip-flop
    outb(0x0C, 0x00);
    // base address (channel 1 -> port 0x02)
    outb(0x02, (uint8_t)(phys & 0xFF));
    outb(0x02, (uint8_t)((phys >> 8) & 0xFF));
    // page register for channel 1
    outb(0x83, (uint8_t)((phys >> 16) & 0xFF));
    // count (channel 1 -> port 0x03)
    outb(0x03, (uint8_t)(count & 0xFF));
    outb(0x03, (uint8_t)((count >> 8) & 0xFF));
    // single mode, address increment, memory->device(read), channel 1
    outb(0x0B, 0x49);
    // unmask channel 1
    outb(0x0A, 0x01);
}

static int sb16_init(void) {
    if (sb16_initialized) return 0;
    if (sb16_failed) return -1;

    // Standard SB16 DSP reset handshake:
    // write 1->0 to reset port, then wait for 0xAA from read data port.
    outb(0x226, 1);
    for (int i = 0; i < 64; i++) io_wait();
    outb(0x226, 0);

    // Wait for 0xAA acknowledge
    int ok = 0;
    for (int i = 0; i < 100000; i++) {
        if (inb(0x22E) & 0x80) {
            if (inb(0x22A) == 0xAA) {
                ok = 1;
            }
            break;
        }
    }

    if (!ok) {
        sb16_failed = 1;
        sb16_report_once("DSP reset handshake failed");
        return -1;
    }

    // Enable DSP speaker output.
    if (sb16_write(0xD1) != 0) {
        sb16_failed = 1;
        sb16_report_once("failed to enable DSP speaker");
        return -1;
    }

    sb16_initialized = 1;
    puts("[sb16] initialized\n");
    return 0;
}

void sound_beep_start(uint32_t freq_hz) {
    if (freq_hz < 20) freq_hz = 20;
    if (freq_hz > 20000) freq_hz = 20000;

    uint16_t divisor = (uint16_t)(1193182U / freq_hz);
    if (divisor == 0) divisor = 1;

    // PIT ch2, lobyte/hibyte, square wave
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)((divisor >> 8) & 0xFF));

    uint8_t val = inb(0x61);
    if ((val & 0x03) != 0x03) {
        outb(0x61, val | 0x03);
    }
}

void sound_beep_stop(void) {
    uint8_t val = inb(0x61) & (uint8_t)~0x03;
    outb(0x61, val);
}

int sound_pcm_play_u8(const uint8_t* data, uint32_t len, uint32_t sample_rate) {
    if (!data || len == 0) return 0;
    if (sample_rate < 4000) sample_rate = 4000;
    if (sample_rate > 44100) sample_rate = 44100;

    if (sb16_init() != 0) return -1;
    if (sb16_prepare_dma_buffer() != 0) return -1;

    if (len > 4096) len = 4096;
    for (uint32_t i = 0; i < len; i++) {
        sb16_dma_buf[i] = data[i];
    }

    uint32_t tc = 256 - (1000000U / sample_rate);
    if (tc > 255) tc = 255;

    // 0x40: set DSP time constant (8-bit sample clock).
    if (sb16_write(0x40) != 0) {
        sb16_report_once("failed to write time constant command");
        return -1;
    }
    if (sb16_write((uint8_t)tc) != 0) {
        sb16_report_once("failed to write time constant value");
        return -1;
    }

    dma_ch1_program(sb16_dma_phys, (uint16_t)len);

    // 0x14: 8-bit DMA DAC (single-cycle)
    if (sb16_write(0x14) != 0) {
        sb16_report_once("failed to start DMA DAC command");
        return -1;
    }
    if (sb16_write((uint8_t)((len - 1) & 0xFF)) != 0) {
        sb16_report_once("failed to write DMA length low");
        return -1;
    }
    if (sb16_write((uint8_t)(((len - 1) >> 8) & 0xFF)) != 0) {
        sb16_report_once("failed to write DMA length high");
        return -1;
    }

    return (int)len;
}
