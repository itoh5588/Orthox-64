#include <stdint.h>
#include "riscv64/boot.h"
#include "riscv64/csr.h"
#include "riscv64/vm.h"

#define RISCV64_SATP_MODE_SV39      (8ULL << 60)
#define RISCV64_SATP_PPN_MASK       ((1ULL << 44) - 1ULL)
#define RISCV64_PAGE_SIZE           4096ULL
#define RISCV64_PAGE_MASK           (~(RISCV64_PAGE_SIZE - 1ULL))

#define RISCV64_SV39_PTE_V          (1ULL << 0)
#define RISCV64_SV39_PTE_R          (1ULL << 1)
#define RISCV64_SV39_PTE_W          (1ULL << 2)
#define RISCV64_SV39_PTE_X          (1ULL << 3)
#define RISCV64_SV39_PTE_U          (1ULL << 4)
#define RISCV64_SV39_PTE_G          (1ULL << 5)
#define RISCV64_SV39_PTE_A          (1ULL << 6)
#define RISCV64_SV39_PTE_D          (1ULL << 7)

static uint64_t g_riscv64_sv39_root[512] __attribute__((aligned(4096)));
static uint64_t g_riscv64_sv39_roots[4][512] __attribute__((aligned(4096)));
static uint64_t g_riscv64_sv39_tables[16][512] __attribute__((aligned(4096)));
static uint8_t g_riscv64_bootstrap_pages[128][RISCV64_PAGE_SIZE] __attribute__((aligned(4096)));
static uint64_t g_riscv64_last_satp;
static uint32_t g_riscv64_sv39_next_table;
static uint32_t g_riscv64_sv39_root_used_mask;
static uint8_t g_riscv64_bootstrap_page_used[128];

extern char __kernel_start[];
extern char __kernel_end[];
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];

static uint64_t riscv64_align_down(uint64_t value) {
    return value & RISCV64_PAGE_MASK;
}

static uint64_t riscv64_align_up(uint64_t value) {
    return (value + RISCV64_PAGE_SIZE - 1ULL) & RISCV64_PAGE_MASK;
}

static uint32_t riscv64_bootstrap_page_used_count(void) {
    uint32_t used = 0;
    for (uint32_t i = 0; i < 128U; i++) {
        if (g_riscv64_bootstrap_page_used[i] != 0) used++;
    }
    return used;
}

static uint64_t riscv64_vm_flags_to_pte(uint64_t flags) {
    uint64_t pte = RISCV64_SV39_PTE_V | RISCV64_SV39_PTE_A;
    if (flags & RISCV64_VM_PAGE_R) pte |= RISCV64_SV39_PTE_R;
    if (flags & RISCV64_VM_PAGE_W) pte |= RISCV64_SV39_PTE_W | RISCV64_SV39_PTE_D;
    if (flags & RISCV64_VM_PAGE_X) pte |= RISCV64_SV39_PTE_X;
    if (flags & RISCV64_VM_PAGE_U) pte |= RISCV64_SV39_PTE_U;
    if (flags & RISCV64_VM_PAGE_G) pte |= RISCV64_SV39_PTE_G;
    return pte;
}

static void riscv64_memzero(void* ptr, uint64_t size) {
    uint8_t* p = (uint8_t*)ptr;
    while (size--) *p++ = 0;
}

static uint64_t riscv64_vm_satp_value(uint64_t root_pa) {
    return RISCV64_SATP_MODE_SV39 | ((root_pa >> 12) & RISCV64_SATP_PPN_MASK);
}

static uint64_t riscv64_sv39_make_nonleaf(uint64_t table_pa) {
    return ((table_pa >> 12) << 10) | RISCV64_SV39_PTE_V;
}

static uint64_t riscv64_sv39_make_leaf_4k(uint64_t phys_base, uint64_t flags) {
    return ((phys_base >> 12) << 10) | flags;
}

static uint64_t* riscv64_sv39_alloc_table(void) {
    uint64_t* table;
    if (g_riscv64_sv39_next_table >= 16U) {
        riscv64_uart_puts("riscv64 vm: page-table pool exhausted\n");
        riscv64_wait_forever();
    }
    table = g_riscv64_sv39_tables[g_riscv64_sv39_next_table++];
    for (int i = 0; i < 512; i++) table[i] = 0;
    return table;
}

static uint64_t* riscv64_sv39_next_level(uint64_t* table, uint64_t index) {
    uint64_t entry = table[index];
    if ((entry & RISCV64_SV39_PTE_V) != 0) {
        uint64_t child_pa = ((entry >> 10) << 12);
        return (uint64_t*)(uintptr_t)child_pa;
    }

    uint64_t* next = riscv64_sv39_alloc_table();
    table[index] = riscv64_sv39_make_nonleaf((uint64_t)(uintptr_t)next);
    return next;
}

static uint64_t* riscv64_vm_root_ptr(uint64_t root_pa) {
    return (uint64_t*)(uintptr_t)root_pa;
}

static uint64_t* riscv64_sv39_walk_create(uint64_t* root, uint64_t virt_addr) {
    uint64_t vpn2 = (virt_addr >> 30) & 0x1ffULL;
    uint64_t vpn1 = (virt_addr >> 21) & 0x1ffULL;
    uint64_t* l1 = riscv64_sv39_next_level(root, vpn2);
    return riscv64_sv39_next_level(l1, vpn1);
}

static uint64_t* riscv64_sv39_walk_leaf(uint64_t* root, uint64_t virt_addr) {
    uint64_t vpn2 = (virt_addr >> 30) & 0x1ffULL;
    uint64_t vpn1 = (virt_addr >> 21) & 0x1ffULL;
    uint64_t vpn0 = (virt_addr >> 12) & 0x1ffULL;
    uint64_t entry = root[vpn2];
    if ((entry & RISCV64_SV39_PTE_V) == 0) return 0;
    uint64_t* l1 = (uint64_t*)(uintptr_t)(((entry >> 10) << 12));
    entry = l1[vpn1];
    if ((entry & RISCV64_SV39_PTE_V) == 0) return 0;
    uint64_t* l0 = (uint64_t*)(uintptr_t)(((entry >> 10) << 12));
    return &l0[vpn0];
}

static void riscv64_sv39_map_page_into(uint64_t* root, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    uint64_t vpn0 = (virt_addr >> 12) & 0x1ffULL;
    uint64_t* l0 = riscv64_sv39_walk_create(root, virt_addr);
    l0[vpn0] = riscv64_sv39_make_leaf_4k(phys_addr, flags);
}

uint64_t riscv64_vm_create_address_space(void) {
    for (uint32_t i = 0; i < 4U; i++) {
        if ((g_riscv64_sv39_root_used_mask & (1U << i)) != 0) continue;
        g_riscv64_sv39_root_used_mask |= (1U << i);
        for (int j = 0; j < 512; j++) {
            g_riscv64_sv39_roots[i][j] = g_riscv64_sv39_root[j];
        }
        return (uint64_t)(uintptr_t)g_riscv64_sv39_roots[i];
    }
    return 0;
}

void riscv64_vm_destroy_address_space(uint64_t root_pa) {
    for (uint32_t i = 0; i < 4U; i++) {
        if ((uint64_t)(uintptr_t)g_riscv64_sv39_roots[i] != root_pa) continue;
        g_riscv64_sv39_root_used_mask &= ~(1U << i);
        for (int j = 0; j < 512; j++) g_riscv64_sv39_roots[i][j] = 0;
        return;
    }
}

uint64_t riscv64_vm_bootstrap_alloc_page(void) {
    for (uint32_t i = 0; i < 128U; i++) {
        if (g_riscv64_bootstrap_page_used[i] != 0) continue;
        g_riscv64_bootstrap_page_used[i] = 1;
        riscv64_memzero(g_riscv64_bootstrap_pages[i], RISCV64_PAGE_SIZE);
        return (uint64_t)(uintptr_t)g_riscv64_bootstrap_pages[i];
    }
    return 0;
}

void riscv64_vm_bootstrap_free_page(uint64_t phys_addr) {
    for (uint32_t i = 0; i < 128U; i++) {
        if ((uint64_t)(uintptr_t)g_riscv64_bootstrap_pages[i] != phys_addr) continue;
        g_riscv64_bootstrap_page_used[i] = 0;
        return;
    }
}

void riscv64_vm_map_page(uint64_t root_pa, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    riscv64_sv39_map_page_into(riscv64_vm_root_ptr(root_pa),
                               riscv64_align_down(virt_addr),
                               riscv64_align_down(phys_addr),
                               riscv64_vm_flags_to_pte(flags));
}

void riscv64_vm_map_range(uint64_t root_pa, uint64_t virt_addr, uint64_t phys_addr, uint64_t size, uint64_t flags) {
    uint64_t va = riscv64_align_down(virt_addr);
    uint64_t pa = riscv64_align_down(phys_addr);
    uint64_t limit = riscv64_align_up(virt_addr + size);
    while (va < limit) {
        riscv64_vm_map_page(root_pa, va, pa, flags);
        va += RISCV64_PAGE_SIZE;
        pa += RISCV64_PAGE_SIZE;
    }
}

uint64_t riscv64_vm_get_phys(uint64_t root_pa, uint64_t virt_addr) {
    uint64_t* pte = riscv64_sv39_walk_leaf(riscv64_vm_root_ptr(root_pa), virt_addr);
    uint64_t entry;
    if (!pte) return 0;
    entry = *pte;
    if ((entry & RISCV64_SV39_PTE_V) == 0) return 0;
    return (((entry >> 10) << 12) | (virt_addr & (RISCV64_PAGE_SIZE - 1ULL)));
}

void riscv64_vm_unmap_page(uint64_t root_pa, uint64_t virt_addr) {
    uint64_t* pte = riscv64_sv39_walk_leaf(riscv64_vm_root_ptr(root_pa), virt_addr);
    if (!pte) return;
    *pte = 0;
    riscv64_sfence_vma();
}

void riscv64_vm_update_page_flags(uint64_t root_pa, uint64_t virt_addr, uint64_t flags) {
    uint64_t* pte = riscv64_sv39_walk_leaf(riscv64_vm_root_ptr(root_pa), virt_addr);
    uint64_t phys;
    if (!pte || (*pte & RISCV64_SV39_PTE_V) == 0) return;
    phys = ((*pte >> 10) << 12);
    *pte = riscv64_sv39_make_leaf_4k(phys, riscv64_vm_flags_to_pte(flags));
    riscv64_sfence_vma();
}

uint64_t riscv64_vm_root_pa(void) {
    return (uint64_t)(uintptr_t)g_riscv64_sv39_root;
}

uint64_t riscv64_vm_kernel_address_space(void) {
    return riscv64_vm_root_pa();
}

void riscv64_vm_activate_address_space(uint64_t root_pa) {
    uint64_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
#ifdef __riscv
    riscv64_uart_puts("  vm activate: 0x");
    riscv64_uart_puthex64(root_pa);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("    text->phys: 0x");
    riscv64_uart_puthex64(riscv64_vm_get_phys(root_pa, (uint64_t)(uintptr_t)__text_start));
    riscv64_uart_puts("\n");
    riscv64_uart_puts("    sp  ->phys: 0x");
    riscv64_uart_puthex64(riscv64_vm_get_phys(root_pa, sp));
    riscv64_uart_puts("\n");
    riscv64_uart_puts("    uart->phys: 0x");
    riscv64_uart_puthex64(riscv64_vm_get_phys(root_pa, RISCV64_QEMU_VIRT_UART0_BASE));
    riscv64_uart_puts("\n");
#endif
    g_riscv64_last_satp = riscv64_vm_satp_value(root_pa);
    riscv64_write_satp(g_riscv64_last_satp);
    riscv64_sfence_vma();
}

uint64_t riscv64_vm_clone_kernel_address_space(void) {
    return riscv64_vm_create_address_space();
}

void riscv64_vm_init(void) {
    const riscv64_boot_info_t* boot = riscv64_boot_info();
    for (int i = 0; i < 512; i++) g_riscv64_sv39_root[i] = 0;
    g_riscv64_sv39_next_table = 0;
    g_riscv64_sv39_root_used_mask = 0;
    for (int i = 0; i < 128; i++) g_riscv64_bootstrap_page_used[i] = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 512; j++) g_riscv64_sv39_roots[i][j] = 0;
    }

    riscv64_vm_map_range(riscv64_vm_root_pa(),
                         (uint64_t)(uintptr_t)__text_start,
                         (uint64_t)(uintptr_t)__text_start,
                         (uint64_t)((uintptr_t)__text_end - (uintptr_t)__text_start),
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_X | RISCV64_VM_PAGE_G);
    riscv64_vm_map_range(riscv64_vm_root_pa(),
                         (uint64_t)(uintptr_t)__rodata_start,
                         (uint64_t)(uintptr_t)__rodata_start,
                         (uint64_t)((uintptr_t)__rodata_end - (uintptr_t)__rodata_start),
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_G);
    riscv64_vm_map_range(riscv64_vm_root_pa(),
                         (uint64_t)(uintptr_t)__data_start,
                         (uint64_t)(uintptr_t)__data_start,
                         (uint64_t)((uintptr_t)__data_end - (uintptr_t)__data_start),
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);
    riscv64_vm_map_range(riscv64_vm_root_pa(),
                         (uint64_t)(uintptr_t)__bss_start,
                         (uint64_t)(uintptr_t)__bss_start,
                         (uint64_t)((uintptr_t)__bss_end - (uintptr_t)__bss_start),
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);

    riscv64_vm_map_range(riscv64_vm_root_pa(),
                         RISCV64_QEMU_VIRT_UART0_BASE,
                         RISCV64_QEMU_VIRT_UART0_BASE,
                         RISCV64_PAGE_SIZE,
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);
    riscv64_vm_map_range(riscv64_vm_root_pa(),
                         RISCV64_QEMU_VIRT_CLINT_BASE,
                         RISCV64_QEMU_VIRT_CLINT_BASE,
                         0x10000ULL,
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);
    riscv64_vm_map_range(riscv64_vm_root_pa(),
                         RISCV64_QEMU_VIRT_PLIC_BASE,
                         RISCV64_QEMU_VIRT_PLIC_BASE,
                         0x400000ULL,
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);

    if (boot && boot->dtb_pa && boot->dtb_size) {
        riscv64_vm_map_range(riscv64_vm_root_pa(),
                             boot->dtb_pa,
                             boot->dtb_pa,
                             boot->dtb_size,
                             RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_G);
    }

    if (boot && boot->first_virtio_mmio_base) {
        riscv64_vm_map_range(riscv64_vm_root_pa(),
                             boot->first_virtio_mmio_base,
                             boot->first_virtio_mmio_base,
                             RISCV64_PAGE_SIZE,
                             RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);
    }

    riscv64_vm_activate_address_space(riscv64_vm_root_pa());

    riscv64_uart_puts("  sv39 satp enabled\n");
    riscv64_uart_puts("  sv39 root: 0x");
    riscv64_uart_puthex64(riscv64_vm_root_pa());
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  kernel: 0x");
    riscv64_uart_puthex64((uint64_t)(uintptr_t)__kernel_start);
    riscv64_uart_puts("..0x");
    riscv64_uart_puthex64((uint64_t)(uintptr_t)__kernel_end);
    riscv64_uart_puts("\n");
}

void riscv64_vm_dump_plan(void) {
    riscv64_uart_puts("riscv64 vm\n");
    riscv64_uart_puts("  satp : 0x");
    riscv64_uart_puthex64(riscv64_read_satp());
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  root : 0x");
    riscv64_uart_puthex64(riscv64_vm_root_pa());
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  aspace slots: 0x");
    riscv64_uart_puthex64((uint64_t)g_riscv64_sv39_root_used_mask);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  page pool : 0x");
    riscv64_uart_puthex64((uint64_t)riscv64_bootstrap_page_used_count());
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  text@ : 0x");
    riscv64_uart_puthex64(riscv64_vm_get_phys(riscv64_vm_root_pa(), (uint64_t)(uintptr_t)__text_start));
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  stvec: 0x");
    riscv64_uart_puthex64(riscv64_read_stvec());
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  sstatus: 0x");
    riscv64_uart_puthex64(riscv64_read_sstatus());
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  sie  : 0x");
    riscv64_uart_puthex64(riscv64_read_sie());
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  sip  : 0x");
    riscv64_uart_puthex64(riscv64_read_sip());
    riscv64_uart_puts("\n");
}

arch_address_space_t arch_vm_kernel_address_space(void) {
    return (arch_address_space_t)riscv64_vm_kernel_address_space();
}

arch_address_space_t arch_vm_create_user_address_space(void) {
    return (arch_address_space_t)riscv64_vm_create_address_space();
}

arch_address_space_t arch_vm_clone_address_space(arch_address_space_t address_space) {
    (void)address_space;
    return (arch_address_space_t)riscv64_vm_clone_kernel_address_space();
}

void arch_vm_destroy_user_address_space(arch_address_space_t address_space) {
    riscv64_vm_destroy_address_space((uint64_t)address_space);
}

void arch_vm_map_page(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    riscv64_vm_map_page((uint64_t)address_space, vaddr, paddr, flags);
}

void arch_vm_map_range(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    riscv64_vm_map_range((uint64_t)address_space, vaddr, paddr, size, flags);
}

uint64_t arch_vm_get_phys(arch_address_space_t address_space, uint64_t vaddr) {
    return riscv64_vm_get_phys((uint64_t)address_space, vaddr);
}

void arch_vm_unmap_page(arch_address_space_t address_space, uint64_t vaddr) {
    riscv64_vm_unmap_page((uint64_t)address_space, vaddr);
}

void arch_vm_update_page_flags(arch_address_space_t address_space, uint64_t vaddr, uint64_t flags) {
    riscv64_vm_update_page_flags((uint64_t)address_space, vaddr, flags);
}
