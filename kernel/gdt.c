#include "gdt.h"
#include <stddef.h>
#include <stdint.h>

static uint64_t gdt[GDT_ENTRY_COUNT] __attribute__((aligned(8)));
static struct gdt_ptr gp;
static struct tss tss_obj __attribute__((aligned(16)));

extern void gdt_flush(uintptr_t);

static uint64_t create_descriptor(uint32_t limit, uint8_t access_right, uint8_t flags) {
    uint64_t desc = 0;
    desc |= (uint64_t)(limit & 0xFFFF);
    desc |= (uint64_t)(access_right) << 40;
    desc |= (uint64_t)(limit & 0xF0000) << 32;
    desc |= (uint64_t)(flags & 0x0F) << 52;
    return desc;
}

void tss_set_stack(uint64_t stack) {
    tss_obj.rsp[0] = stack;
}

void tss_set_ist(int index, uint64_t stack) {
    if (index >= 1 && index <= 7) {
        tss_obj.ist[index - 1] = stack;
    }
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[GDT_KERNEL_CODE_IDX] = create_descriptor(0xFFFFF, 0x9A, 0x0A);
    gdt[GDT_KERNEL_DATA_IDX] = create_descriptor(0xFFFFF, 0x92, 0x0C);
    gdt[GDT_USER_DATA_IDX]   = create_descriptor(0xFFFFF, 0xF2, 0x0C);
    gdt[GDT_USER_CODE_IDX]   = create_descriptor(0xFFFFF, 0xFA, 0x0A);

    uintptr_t tss_addr = (uintptr_t)&tss_obj;
    uint32_t tss_limit = sizeof(struct tss) - 1;

    struct tss_entry* tss_desc = (struct tss_entry*)&gdt[GDT_TSS_IDX];
    tss_desc->length = tss_limit & 0xFFFF;
    tss_desc->base_low = tss_addr & 0xFFFF;
    tss_desc->base_mid = (tss_addr >> 16) & 0xFF;
    tss_desc->flags1 = 0x89;
    tss_desc->flags2 = (tss_limit >> 16) & 0x0F;
    tss_desc->base_high_mid = (tss_addr >> 24) & 0xFF;
    tss_desc->base_high = (uint32_t)(tss_addr >> 32);
    tss_desc->reserved = 0;

    for (size_t i = 0; i < sizeof(struct tss); i++) ((uint8_t*)&tss_obj)[i] = 0;
    tss_obj.iopb_offset = (uint16_t)sizeof(struct tss);

    gp.limit = sizeof(gdt) - 1;
    gp.base = (uintptr_t)&gdt;

    gdt_flush((uintptr_t)&gp);

    __asm__ volatile("ltr %%ax" : : "a"((uint16_t)GDT_TSS));
}
