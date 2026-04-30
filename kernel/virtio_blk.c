#include <stdint.h>
#include <stddef.h>
#include "virtio_blk.h"
#include "virtio.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "wait.h"
#include "pic.h"
#include "task.h"
#include "bottom_half.h"
#include "irq.h"

void puts(const char* s);
void puthex(uint64_t v);

#define VIRTIO_BLK_QUEUE 0
#define VIRTIO_BLK_TIMEOUT_MS 5000ULL

static struct virtio_queue g_vblk_q;
static uint16_t g_vblk_iobase = 0;
static int g_vblk_ready = 0;
static int g_vblk_irq_line = -1;
static int g_vblk_irq_enabled = 0;
static uint64_t g_vblk_capacity = 0;
static struct completion g_vblk_completion;
static struct wait_queue g_vblk_idle_wait;
static int g_vblk_busy = 0;

/* Request resources (Header and Status byte) */
static struct virtio_blk_req* g_req_hdr = NULL;
static uint64_t g_req_hdr_phys = 0;
static uint8_t* g_req_status = NULL;
static uint64_t g_req_status_phys = 0;

static int virtio_blk_irq(int irq, void* ctx);

static int vblk_reclaim_used(void) {
    if (!g_vblk_ready) return -1;
    if (g_vblk_q.last_used_idx == g_vblk_q.used->idx) return 0;
    while (g_vblk_q.last_used_idx != g_vblk_q.used->idx) {
        g_vblk_q.last_used_idx++;
    }
    return *g_req_status == VIRTIO_BLK_S_OK ? 1 : -1;
}

static void vblk_complete_bottom_half(void* arg) {
    int status;
    (void)arg;
    status = vblk_reclaim_used();
    if (status == 0) return;
    complete_status(&g_vblk_completion,
                    status > 0 ? 0 : -1);
}

static int vblk_not_busy(void* arg) {
    (void)arg;
    return !g_vblk_busy;
}

static int vblk_acquire_request(void) {
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_vblk_idle_wait.lock);
        if (!g_vblk_busy) {
            g_vblk_busy = 1;
            spin_unlock_irqrestore(&g_vblk_idle_wait.lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&g_vblk_idle_wait.lock, flags);

        if (!get_current_task()) {
            while (g_vblk_busy) {
                __asm__ volatile("pause");
            }
            continue;
        }
        if (wait_event(&g_vblk_idle_wait, vblk_not_busy, 0) < 0) {
            return -1;
        }
    }
}

static void vblk_release_request(void) {
    uint64_t flags = spin_lock_irqsave(&g_vblk_idle_wait.lock);
    g_vblk_busy = 0;
    spin_unlock_irqrestore(&g_vblk_idle_wait.lock, flags);
    wake_up_one(&g_vblk_idle_wait);
}

static void vblk_fail(const char* msg) {
    if (g_vblk_iobase) {
        outb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_STATUS), VIRTIO_STATUS_FAILED);
    }
    puts("[vblk] virtio-blk init failed: ");
    puts(msg);
    puts("\r\n");
}

static void vblk_disable_after_timeout(void) {
    g_vblk_ready = 0;
    if (g_vblk_irq_line >= 0) {
        pic_mask_irq(g_vblk_irq_line);
    }
    g_vblk_irq_enabled = 0;
    vblk_fail("request timeout");
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
    wait_queue_init(&g_vblk_idle_wait);
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
        (void)irq_register_legacy(g_vblk_irq_line, virtio_blk_irq, 0);
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

static int virtio_blk_irq(int irq, void* ctx) {
    uint8_t isr;
    (void)ctx;
    if (!g_vblk_ready || irq != g_vblk_irq_line) return 0;
    isr = inb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_ISR));
    if ((isr & 0x1U) == 0) return 1;
    if (bottom_half_enqueue(vblk_complete_bottom_half, 0) < 0) {
        vblk_complete_bottom_half(0);
    }
    return 1;
}

static int vblk_request(uint32_t type, uint64_t sector, void* buf, uint32_t len) {
    int ret = -1;
    if (!g_vblk_ready) return -1;
    if (vblk_acquire_request() < 0) return -1;

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

    if (g_vblk_irq_enabled && get_current_task()) {
        int status = 0;
        int wait_ret = wait_for_completion_timeout_status(&g_vblk_completion,
                                                          VIRTIO_BLK_TIMEOUT_MS,
                                                          &status);
        if (wait_ret == 0) {
            vblk_disable_after_timeout();
            goto out;
        }
        if (wait_ret < 0 || status < 0) {
            goto out;
        }
    } else {
        int status;
        /* Legacy fallback for environments without a usable PCI IRQ line. */
        while (g_vblk_q.last_used_idx == g_vblk_q.used->idx) {
            __asm__ volatile("pause");
        }
        status = vblk_reclaim_used();
        if (status < 0) goto out;
    }

    if (*g_req_status != VIRTIO_BLK_S_OK) {
        goto out;
    }

    ret = 0;
out:
    vblk_release_request();
    return ret;
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
