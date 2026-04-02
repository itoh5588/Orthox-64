#include <stddef.h>
#include <stdint.h>
#include "pmm.h"
#include "riscv64/vm.h"

void* pmm_alloc(size_t pages) {
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

void pmm_free(void* addr, size_t pages) {
    uint64_t base = (uint64_t)(uintptr_t)addr;
    if (!addr || pages == 0) return;
    for (size_t i = 0; i < pages; i++) {
        riscv64_vm_bootstrap_free_page(base + i * PAGE_SIZE);
    }
}
