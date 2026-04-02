#include <stdint.h>
#include <stddef.h>
#include "arch_mmu.h"
#include "arch_trap.h"
#include "arch_vm.h"
#include "arch_vm_backend.h"
#include "vmm.h"
#include "pmm.h"
#include "limine.h"

extern volatile struct limine_hhdm_request hhdm_request;

uint64_t g_hhdm_offset = 0;
static uint64_t* kernel_pml4;

void vmm_init(void) {
    g_hhdm_offset = hhdm_request.response->offset;

    void* pml4_phys = pmm_alloc(1);
    kernel_pml4 = (uint64_t*)PHYS_TO_VIRT(pml4_phys);
    for (int i = 0; i < 512; i++) kernel_pml4[i] = 0;
    arch_vm_backend_init(kernel_pml4);

    vmm_map_range(kernel_pml4, 0, 0, 0x100000000ULL, PTE_PRESENT | PTE_WRITABLE);
    
    extern volatile struct limine_kernel_address_request kernel_address_request;
    if (kernel_address_request.response) {
        struct limine_kernel_address_response* kaddr = kernel_address_request.response;
        vmm_map_range(kernel_pml4, kaddr->virtual_base, kaddr->physical_base, 0x2000000, PTE_PRESENT | PTE_WRITABLE);
    }
}

arch_address_space_t arch_vm_kernel_address_space(void) {
    return vmm_get_kernel_pml4_phys();
}

uint64_t* arch_vm_address_space_root(arch_address_space_t address_space) {
    if (!address_space) return 0;
    return (uint64_t*)PHYS_TO_VIRT((void*)address_space);
}

arch_address_space_t arch_vm_create_user_address_space(void) {
    void* root_phys = pmm_alloc(1);
    if (!root_phys) return 0;
    uint64_t* root = (uint64_t*)PHYS_TO_VIRT(root_phys);
    uint64_t* kernel_root = vmm_get_kernel_pml4();
    for (int i = 0; i < 512; i++) root[i] = (i >= 256) ? kernel_root[i] : 0;
    return (arch_address_space_t)(uint64_t)root_phys;
}

arch_address_space_t arch_vm_clone_address_space(arch_address_space_t address_space) {
    return arch_vm_backend_clone_address_space(arch_vm_address_space_root(address_space));
}

void arch_vm_destroy_user_address_space(arch_address_space_t address_space) {
    arch_vm_backend_destroy_user_address_space(address_space);
}

void arch_vm_map_page(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    vmm_map_page(arch_vm_address_space_root(address_space), vaddr, paddr, flags);
}

void arch_vm_map_range(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    vmm_map_range(arch_vm_address_space_root(address_space), vaddr, paddr, size, flags);
}

uint64_t arch_vm_get_phys(arch_address_space_t address_space, uint64_t vaddr) {
    return vmm_get_phys(arch_vm_address_space_root(address_space), vaddr);
}

void arch_vm_unmap_page(arch_address_space_t address_space, uint64_t vaddr) {
    arch_vm_backend_unmap_page(arch_vm_address_space_root(address_space), vaddr);
}

void arch_vm_update_page_flags(arch_address_space_t address_space, uint64_t vaddr, uint64_t new_flags) {
    arch_vm_backend_update_page_flags(arch_vm_address_space_root(address_space), vaddr, new_flags);
}

void vmm_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    arch_vm_backend_map_page(pml4, vaddr, paddr, flags);
}

void vmm_map_range(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    arch_vm_backend_map_range(pml4, vaddr, paddr, size, flags);
}

uint64_t vmm_get_phys(uint64_t* pml4, uint64_t vaddr) {
    return arch_vm_backend_get_phys(pml4, vaddr);
}

void vmm_activate(uint64_t* pml4) {
    uint64_t pml4_phys = VIRT_TO_PHYS(pml4);
    arch_mmu_write_address_space(pml4_phys);
}

uint64_t* vmm_get_kernel_pml4(void) {
    return kernel_pml4;
}

uint64_t vmm_get_kernel_pml4_phys(void) {
    return VIRT_TO_PHYS(kernel_pml4);
}

uint64_t vmm_copy_pml4(uint64_t* old_pml4) {
    return arch_vm_backend_clone_address_space(old_pml4);
}

void vmm_free_user_pml4(uint64_t pml4_phys) {
    arch_vm_backend_destroy_user_address_space(pml4_phys);
}

void vmm_page_fault_handler(arch_interrupt_frame_t* frame) {
    arch_vm_backend_handle_page_fault(frame);
}
