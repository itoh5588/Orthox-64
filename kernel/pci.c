#include <stdint.h>
#include "pci.h"

void puts(const char* s);
void puthex(uint64_t v);

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = (1U << 31)
        | ((uint32_t)bus << 16)
        | ((uint32_t)device << 11)
        | ((uint32_t)function << 8)
        | (offset & 0xFC);
    outl(0xCF8, address);
    return inl(0xCFC);
}

static void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = (1U << 31)
        | ((uint32_t)bus << 16)
        | ((uint32_t)device << 11)
        | ((uint32_t)function << 8)
        | (offset & 0xFC);
    outl(0xCF8, address);
    outl(0xCFC, value);
}

static uint16_t pci_vendor_id(uint8_t bus, uint8_t device, uint8_t function) {
    return (uint16_t)(pci_config_read32(bus, device, function, 0x00) & 0xFFFF);
}

static int g_has_xhci = 0;
static struct pci_device_info g_xhci;

static void pci_probe_function(uint8_t bus, uint8_t device, uint8_t function) {
    uint16_t vendor = pci_vendor_id(bus, device, function);
    if (vendor == 0xFFFF) return;

    uint32_t id = pci_config_read32(bus, device, function, 0x00);
    uint32_t class_reg = pci_config_read32(bus, device, function, 0x08);
    uint32_t hdr_reg = pci_config_read32(bus, device, function, 0x0C);
    uint32_t irq_reg = pci_config_read32(bus, device, function, 0x3C);

    struct pci_device_info info;
    info.bus = bus;
    info.device = device;
    info.function = function;
    info.vendor_id = (uint16_t)(id & 0xFFFF);
    info.device_id = (uint16_t)((id >> 16) & 0xFFFF);
    info.class_code = (uint8_t)((class_reg >> 24) & 0xFF);
    info.subclass = (uint8_t)((class_reg >> 16) & 0xFF);
    info.prog_if = (uint8_t)((class_reg >> 8) & 0xFF);
    info.header_type = (uint8_t)((hdr_reg >> 16) & 0xFF);
    info.irq_line = (uint8_t)(irq_reg & 0xFF);

    // Serial Bus Controller / USB / xHCI
    if (!g_has_xhci && info.class_code == 0x0C && info.subclass == 0x03 && info.prog_if == 0x30) {
        g_xhci = info;
        g_has_xhci = 1;
        puts("[pci] xHCI found: vendor=0x");
        puthex(info.vendor_id);
        puts(" device=0x");
        puthex(info.device_id);
        puts(" bdf=");
        puthex(((uint64_t)bus << 16) | ((uint64_t)device << 8) | function);
        puts("\r\n");
    }
}

void pci_init(void) {
    g_has_xhci = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint16_t vendor = pci_vendor_id((uint8_t)bus, dev, 0);
            if (vendor == 0xFFFF) continue;

            uint32_t hdr_reg = pci_config_read32((uint8_t)bus, dev, 0, 0x0C);
            uint8_t header_type = (uint8_t)((hdr_reg >> 16) & 0xFF);
            uint8_t fn_count = (header_type & 0x80) ? 8 : 1;
            for (uint8_t fn = 0; fn < fn_count; fn++) {
                pci_probe_function((uint8_t)bus, dev, fn);
            }
        }
    }
    if (!g_has_xhci) {
        puts("[pci] xHCI not found\r\n");
    }
}

int pci_find_xhci(struct pci_device_info* out) {
    if (!g_has_xhci || !out) return -1;
    *out = g_xhci;
    return 0;
}

uint64_t pci_get_bar0_mmio(const struct pci_device_info* dev) {
    if (!dev) return 0;

    uint32_t bar0 = pci_config_read32(dev->bus, dev->device, dev->function, 0x10);
    if (bar0 & 0x1) {
        return 0;
    }

    uint64_t base = (uint64_t)(bar0 & 0xFFFFFFF0U);
    uint8_t bar_type = (uint8_t)((bar0 >> 1) & 0x3);
    if (bar_type == 0x2) {
        uint32_t bar1 = pci_config_read32(dev->bus, dev->device, dev->function, 0x14);
        base |= ((uint64_t)bar1 << 32);
    }
    return base;
}

void pci_enable_mmio_busmaster(const struct pci_device_info* dev) {
    if (!dev) return;
    uint32_t reg = pci_config_read32(dev->bus, dev->device, dev->function, 0x04);
    // Command register bits: bit1 Memory Space, bit2 Bus Master.
    uint32_t new_reg = reg | (1U << 1) | (1U << 2);
    if (new_reg != reg) {
        pci_config_write32(dev->bus, dev->device, dev->function, 0x04, new_reg);
    }
}
