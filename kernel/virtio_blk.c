#include <stdint.h>
#include <stddef.h>
#include "virtio_blk.h"
#include "virtio.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "wait.h"
#include "pic.h"

void puts(const char* s);
void puthex(uint64_t v);

#define VIRTIO_BLK_QUEUE 0

static struct virtio_queue g_vblk_q;
static uint16_t g_vblk_iobase = 0;
static int g_vblk_ready = 0;
static int g_vblk_irq_line = -1;
static int g_vblk_irq_enabled = 0;
static uint64_t g_vblk_capacity = 0;
static struct completion g_vblk_completion;

/* Request resources (Header and Status byte) */
static struct virtio_blk_req* g_req_hdr = NULL;
static uint64_t g_req_hdr_phys = 0;
static uint8_t* g_req_status = NULL;
static uint64_t g_req_status_phys = 0;

static void vblk_poll_complete(void) {
    if (!g_vblk_ready) return;
    if (g_vblk_q.last_used_idx == g_vblk_q.used->idx) return;
    while (g_vblk_q.last_used_idx != g_vblk_q.used->idx) {
        g_vblk_q.last_used_idx++;
    }
    complete_status(&g_vblk_completion,
                    *g_req_status == VIRTIO_BLK_S_OK ? 0 : -1);
}

static void vblk_fail(const char* msg) {
    if (g_vblk_iobase) {
        outb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_STATUS), VIRTIO_STATUS_FAILED);
    }
    puts("[vblk] virtio-blk init failed: ");
    puts(msg);
    puts("\r\n");
}

int virtio_blk_init(void) {
    if (g_vblk_ready) return 0;

    struct pci_device_info dev;
    if (pci_find_virtio_blk(&dev) < 0) {
        return -1;
    }

    g_vblk_iobase = pci_get_bar0_iobase(&dev);
    if (g_vblk_iobase == 0) {
        return -1;
    }

    pci_enable_io_busmaster(&dev);
    init_completion(&g_vblk_completion);
    g_vblk_irq_line = (dev.irq_line <= 15) ? (int)dev.irq_line : -1;

    /* 1. Reset device */
    outb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_STATUS), 0);
    /* 2. ACKNOWLEDGE */
    outb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_STATUS), VIRTIO_STATUS_ACKNOWLEDGE);
    /* 3. DRIVER */
    outb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_STATUS), VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 4. Feature negotiation (Skip for legacy) */
    (void)inl((uint16_t)(g_vblk_iobase + VIRTIO_PCI_HOST_FEATURES));
    outl((uint16_t)(g_vblk_iobase + VIRTIO_PCI_GUEST_FEATURES), 0);

    /* 5. Initialize Virtqueue */
    if (virtio_virtq_init(g_vblk_iobase, VIRTIO_BLK_QUEUE, &g_vblk_q) < 0) {
        vblk_fail("queue init");
        return -1;
    }

    /* Allocate request header and status byte */
    void* req_page_phys = pmm_alloc(1);
    if (!req_page_phys) {
        vblk_fail("request buffer allocation");
        return -1;
    }
    g_req_hdr = (struct virtio_blk_req*)PHYS_TO_VIRT(req_page_phys);
    g_req_hdr_phys = (uint64_t)req_page_phys;
    g_req_status = (uint8_t*)g_req_hdr + sizeof(struct virtio_blk_req);
    g_req_status_phys = g_req_hdr_phys + sizeof(struct virtio_blk_req);

    /* 6. Read capacity from config space */
    struct virtio_blk_config cfg;
    for (size_t i = 0; i < sizeof(cfg); i++) {
        ((uint8_t*)&cfg)[i] = inb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_CONFIG + i));
    }
    g_vblk_capacity = cfg.capacity;

    /* 7. DRIVER_OK */
    outb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_STATUS),
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    g_vblk_ready = 1;
    if (g_vblk_irq_line >= 0) {
        pic_unmask_irq(g_vblk_irq_line);
        g_vblk_irq_enabled = 1;
    }
    puts("[vblk] virtio-blk ready: capacity=");
    puthex(g_vblk_capacity);
    puts(" sectors (");
    puthex(g_vblk_capacity * 512 / 1024 / 1024);
    puts(" MiB) io=0x");
    puthex(g_vblk_iobase);
    puts(" irq=0x");
    puthex((uint64_t)(uint32_t)g_vblk_irq_line);
    puts("\r\n");

    return 0;
}

int virtio_blk_irq(int irq) {
    uint8_t isr;
    if (!g_vblk_ready || irq != g_vblk_irq_line) return 0;
    isr = inb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_ISR));
    if ((isr & 0x1U) == 0) return 1;
    vblk_poll_complete();
    return 1;
}

static int vblk_request(uint32_t type, uint64_t sector, void* buf, uint32_t len) {
    if (!g_vblk_ready) return -1;

    /* Use descriptors 0, 1, 2 */
    /* desc[0]: header */
    g_req_hdr->type = type;
    g_req_hdr->reserved = 0;
    g_req_hdr->sector = sector;

    g_vblk_q.desc[0].addr = g_req_hdr_phys;
    g_vblk_q.desc[0].len = sizeof(struct virtio_blk_req);
    g_vblk_q.desc[0].flags = VRING_DESC_F_NEXT;
    g_vblk_q.desc[0].next = 1;

    /* desc[1]: data */
    g_vblk_q.desc[1].addr = VIRT_TO_PHYS(buf);
    g_vblk_q.desc[1].len = len;
    g_vblk_q.desc[1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    g_vblk_q.desc[1].next = 2;

    /* desc[2]: status */
    *g_req_status = 0xFF; /* Initialize with non-zero value */
    g_vblk_q.desc[2].addr = g_req_status_phys;
    g_vblk_q.desc[2].len = 1;
    g_vblk_q.desc[2].flags = VRING_DESC_F_WRITE; /* Device writes to this */
    g_vblk_q.desc[2].next = 0;

    /* Put head into avail ring */
    reinit_completion(&g_vblk_completion);
    g_vblk_q.avail->ring[g_vblk_q.avail->idx % g_vblk_q.queue_size] = 0;
    __sync_synchronize();
    g_vblk_q.avail->idx++;

    /* Kick */
    virtio_kick(g_vblk_iobase, VIRTIO_BLK_QUEUE);

    if (g_vblk_irq_enabled) {
        if (wait_for_completion_status(&g_vblk_completion) < 0) {
            return -1;
        }
    } else {
        /* Legacy fallback for environments without a usable PCI IRQ line. */
        while (g_vblk_q.last_used_idx == g_vblk_q.used->idx) {
            __asm__ volatile("pause");
        }
        g_vblk_q.last_used_idx++;
    }

    if (*g_req_status != VIRTIO_BLK_S_OK) {
        return -1;
    }

    return 0;
}

int virtio_blk_read(uint64_t sector, void* buf, uint32_t count) {
    return vblk_request(VIRTIO_BLK_T_IN, sector, buf, count * 512);
}

int virtio_blk_write(uint64_t sector, const void* buf, uint32_t count) {
    return vblk_request(VIRTIO_BLK_T_OUT, sector, (void*)buf, count * 512);
}

uint64_t virtio_blk_capacity(void) {
    return g_vblk_capacity;
}
