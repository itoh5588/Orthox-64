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

static void pci_fill_info(struct pci_device_info* info,
                          uint8_t bus,
                          uint8_t device,
                          uint8_t function,
                          uint32_t id,
                          uint32_t class_reg,
                          uint32_t hdr_reg,
                          uint32_t irq_reg) {
    info->bus = bus;
    info->device = device;
    info->function = function;
    info->vendor_id = (uint16_t)(id & 0xFFFF);
    info->device_id = (uint16_t)((id >> 16) & 0xFFFF);
    info->class_code = (uint8_t)((class_reg >> 24) & 0xFF);
    info->subclass = (uint8_t)((class_reg >> 16) & 0xFF);
    info->prog_if = (uint8_t)((class_reg >> 8) & 0xFF);
    info->header_type = (uint8_t)((hdr_reg >> 16) & 0xFF);
    info->irq_line = (uint8_t)(irq_reg & 0xFF);
}

static int pci_info_matches(const struct pci_device_info* info,
                            int vendor_id,
                            int device_id,
                            int class_code,
                            int subclass,
                            int prog_if) {
    if (!info) return 0;
    if (vendor_id >= 0 && info->vendor_id != (uint16_t)vendor_id) return 0;
    if (device_id >= 0 && info->device_id != (uint16_t)device_id) return 0;
    if (class_code >= 0 && info->class_code != (uint8_t)class_code) return 0;
    if (subclass >= 0 && info->subclass != (uint8_t)subclass) return 0;
    if (prog_if >= 0 && info->prog_if != (uint8_t)prog_if) return 0;
    return 1;
}

static int g_has_xhci = 0;
static struct pci_device_info g_xhci;
static int g_has_virtio_net = 0;
static struct pci_device_info g_virtio_net;
static int g_has_virtio_blk = 0;
static struct pci_device_info g_virtio_blk;

static void pci_probe_function(uint8_t bus, uint8_t device, uint8_t function) {
    uint16_t vendor = pci_vendor_id(bus, device, function);
    if (vendor == 0xFFFF) return;

    uint32_t id = pci_config_read32(bus, device, function, 0x00);
    uint32_t class_reg = pci_config_read32(bus, device, function, 0x08);
    uint32_t hdr_reg = pci_config_read32(bus, device, function, 0x0C);
    uint32_t irq_reg = pci_config_read32(bus, device, function, 0x3C);

    struct pci_device_info info;
    pci_fill_info(&info, bus, device, function, id, class_reg, hdr_reg, irq_reg);

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

    if (!g_has_virtio_net && info.vendor_id == 0x1AF4 && info.class_code == 0x02) {
        g_virtio_net = info;
        g_has_virtio_net = 1;
        puts("[pci] virtio-net found: vendor=0x");
        puthex(info.vendor_id);
        puts(" device=0x");
        puthex(info.device_id);
        puts(" bdf=");
        puthex(((uint64_t)bus << 16) | ((uint64_t)device << 8) | function);
        puts("\r\n");
    }

    if (!g_has_virtio_blk && info.vendor_id == 0x1AF4 && info.device_id == 0x1001) {
        g_virtio_blk = info;
        g_has_virtio_blk = 1;
        puts("[pci] virtio-blk found: vendor=0x");
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
    g_has_virtio_net = 0;
    g_has_virtio_blk = 0;
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

int pci_find_device(struct pci_device_info* out,
                    int vendor_id,
                    int device_id,
                    int class_code,
                    int subclass,
                    int prog_if) {
    if (!out) return -1;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint16_t vendor = pci_vendor_id((uint8_t)bus, dev, 0);
            if (vendor == 0xFFFF) continue;

            uint32_t hdr_reg = pci_config_read32((uint8_t)bus, dev, 0, 0x0C);
            uint8_t header_type = (uint8_t)((hdr_reg >> 16) & 0xFF);
            uint8_t fn_count = (header_type & 0x80) ? 8 : 1;

            for (uint8_t fn = 0; fn < fn_count; fn++) {
                uint16_t fn_vendor = pci_vendor_id((uint8_t)bus, dev, fn);
                if (fn_vendor == 0xFFFF) continue;

                uint32_t id = pci_config_read32((uint8_t)bus, dev, fn, 0x00);
                uint32_t class_reg = pci_config_read32((uint8_t)bus, dev, fn, 0x08);
                uint32_t fn_hdr_reg = pci_config_read32((uint8_t)bus, dev, fn, 0x0C);
                uint32_t irq_reg = pci_config_read32((uint8_t)bus, dev, fn, 0x3C);
                struct pci_device_info info;
                pci_fill_info(&info, (uint8_t)bus, dev, fn, id, class_reg, fn_hdr_reg, irq_reg);

                if (!pci_info_matches(&info, vendor_id, device_id, class_code, subclass, prog_if)) {
                    continue;
                }

                *out = info;
                return 0;
            }
        }
    }

    return -1;
}

int pci_find_xhci(struct pci_device_info* out) {
    if (!out) return -1;
    if (g_has_xhci) {
        *out = g_xhci;
        return 0;
    }
    return pci_find_device(out, -1, -1, 0x0C, 0x03, 0x30);
}

int pci_find_virtio_net(struct pci_device_info* out) {
    if (!out) return -1;
    if (g_has_virtio_net) {
        *out = g_virtio_net;
        return 0;
    }
    return pci_find_device(out, 0x1AF4, -1, 0x02, -1, -1);
}

int pci_find_virtio_blk(struct pci_device_info* out) {
    if (!out) return -1;
    if (g_has_virtio_blk) {
        *out = g_virtio_blk;
        return 0;
    }
    return pci_find_device(out, 0x1AF4, 0x1001, -1, -1, -1);
}

uint32_t pci_get_bar_raw(const struct pci_device_info* dev, uint8_t bar_index) {
    if (!dev) return 0;
    if (bar_index >= 6) return 0;
    return pci_config_read32(dev->bus, dev->device, dev->function, (uint8_t)(0x10 + bar_index * 4));
}

uint64_t pci_get_bar_mmio(const struct pci_device_info* dev, uint8_t bar_index) {
    uint32_t bar = pci_get_bar_raw(dev, bar_index);
    if (bar == 0) return 0;
    if (bar & 0x1) {
        return 0;
    }

    uint64_t base = (uint64_t)(bar & 0xFFFFFFF0U);
    uint8_t bar_type = (uint8_t)((bar >> 1) & 0x3);
    if (bar_type == 0x2 && bar_index < 5) {
        uint32_t bar_hi = pci_get_bar_raw(dev, (uint8_t)(bar_index + 1));
        base |= ((uint64_t)bar_hi << 32);
    }
    return base;
}

uint16_t pci_get_bar_iobase(const struct pci_device_info* dev, uint8_t bar_index) {
    uint32_t bar = pci_get_bar_raw(dev, bar_index);
    if (!(bar & 0x1)) return 0;
    return (uint16_t)(bar & 0xFFFCU);
}

uint64_t pci_get_bar0_mmio(const struct pci_device_info* dev) {
    return pci_get_bar_mmio(dev, 0);
}

uint16_t pci_get_bar0_iobase(const struct pci_device_info* dev) {
    return pci_get_bar_iobase(dev, 0);
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

void pci_enable_io_busmaster(const struct pci_device_info* dev) {
    if (!dev) return;
    uint32_t reg = pci_config_read32(dev->bus, dev->device, dev->function, 0x04);
    uint32_t new_reg = reg | (1U << 0) | (1U << 2);
    if (new_reg != reg) {
        pci_config_write32(dev->bus, dev->device, dev->function, 0x04, new_reg);
    }
}
