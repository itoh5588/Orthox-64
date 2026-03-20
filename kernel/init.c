#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "pmm.h"
#include "gdt.h"
#include "vmm.h"
#include "idt.h"
#include "syscall.h"
#include "elf64.h"
#include "task.h"
#include "smp.h"
#include "lapic.h"
#include "pci.h"
#include "net.h"
#include "usb.h"

LIMINE_BASE_REVISION(0);

volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0
};

extern void sys_brk_init(uint64_t initial_break);

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %b0, %w1" : : "a"(val), "Nd"(port) );
}

static void init_serial(void) {
    outb(0x3f8 + 1, 0x00); // Disable all interrupts
    outb(0x3f8 + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(0x3f8 + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(0x3f8 + 1, 0x00); //                  (hi byte)
    outb(0x3f8 + 3, 0x03); // 8 bits, no parity, one stop bit (DLAB=0)
    outb(0x3f8 + 1, 0x01); // Enable RX interrupt (DLAB must be 0)
    outb(0x3f8 + 4, 0x0B); // IRQs enabled, RTS/DTR set
}

void puts(const char *s) {
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == '\n') outb(0x3f8, '\r');
        outb(0x3f8, s[i]);
    }
}

void puthex(uint64_t v) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) {
        outb(0x3f8, hex[(v >> i) & 0xF]);
    }
}

static void putdec(uint64_t v) {
    char buf[21];
    int i = 0;
    if (v == 0) {
        outb(0x3f8, '0');
        return;
    }
    while (v && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--) outb(0x3f8, buf[i]);
}

static void enable_sse(void) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 |= (1ULL << 1);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);
    cr4 |= (1ULL << 10);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

static void enable_paging_features(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 16); // WP (Write Protect)
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

void _start(void) {
    init_serial();
    puts("\r\n--- OrthOS-64 Boot ---\r\n");

    if (memmap_request.response && hhdm_request.response && kernel_address_request.response) {
        pmm_init();
        gdt_init();
        idt_init();
        enable_sse();
        enable_paging_features();
        uint64_t cr0_val;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0_val));
        puts("CR0 value: 0x"); puthex(cr0_val); puts("\r\n");
        syscall_init();

        vmm_init();
        extern void pic_init(void);
        pic_init();
        lapic_init();
        pci_init();
        net_init();
        usb_init();
        smp_init(smp_request.response);
        puts("SMP CPUs detected: ");
        putdec(smp_get_cpu_count());
        puts("\r\n");

        uint64_t* pml4 = vmm_get_kernel_pml4();
        struct limine_kernel_address_response* kaddr = kernel_address_request.response;
        
        vmm_map_range(pml4, kaddr->virtual_base, kaddr->physical_base, 0x2000000, PTE_PRESENT | PTE_WRITABLE);
        vmm_map_range(pml4, hhdm_request.response->offset, 0, 0x100000000ULL, PTE_PRESENT | PTE_WRITABLE);
        
        vmm_activate(pml4);
        task_init();

        if (module_request.response && module_request.response->module_count > 0) {
            struct limine_file* module = NULL;
            for (uint64_t i = 0; i < module_request.response->module_count; i++) {
                struct limine_file* m = module_request.response->modules[i];
                const char* path = m->path;
                int len_f = 0; while(path[len_f]) len_f++;
                const char* suffix = "sh.elf";
                int len_s = 0; while(suffix[len_s]) len_s++;
                int match = 1;
                if (len_f >= len_s) {
                    for (int j = 0; j < len_s; j++) {
                        if (path[len_f - len_s + j] != suffix[j]) match = 0;
                    }
                } else match = 0;
                
                if (match) {
                    module = m;
                    break;
                }
            }

            if (module) {
                // 最初は1つのタスクだけ作成
                struct task* user_task = task_create(0, 0);
                uint64_t* user_pml4 = (uint64_t*)PHYS_TO_VIRT(user_task->ctx.cr3);

                struct elf_info info = elf_load(user_pml4, module->address);
                if (info.entry) {
                    char* argv[] = { "sh", NULL };
                    char* envp[] = { "PATH=/bin:/bin-musl", "HOME=/", NULL };
                    if (task_prepare_initial_user_stack(user_pml4, user_task, &info, argv, envp) < 0) {
                        puts("Failed to prepare initial user stack\r\n");
                        while(1);
                    }
                    user_task->heap_break = info.max_vaddr;
                    user_task->user_entry = (uint64_t)info.entry;

                    puts("Starting first user task...\r\n");
                    __asm__ volatile("sti");
                    schedule();
                }
            } else {
                puts("user_test.elf not found!\r\n");
            }
        }
    }

    puts("Idle task running.\r\n");
    while(1) {
        net_poll();
        __asm__ volatile("hlt");
        schedule();
    }
}
