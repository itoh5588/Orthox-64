#include <stdint.h>
#include <stddef.h>
#include "virtio_net.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"

void puts(const char* s);
void puthex(uint64_t v);

#define VIRTIO_PCI_VENDOR_ID 0x1AF4
#define VIRTIO_PCI_DEVICE_NET_MIN 0x1000
#define VIRTIO_PCI_DEVICE_NET_MAX 0x107F

#define VIRTIO_PCI_REG_DEVICE_FEATURES 0x00
#define VIRTIO_PCI_REG_GUEST_FEATURES  0x04
#define VIRTIO_PCI_REG_QUEUE_PFN       0x08
#define VIRTIO_PCI_REG_QUEUE_NUM       0x0C
#define VIRTIO_PCI_REG_QUEUE_SEL       0x0E
#define VIRTIO_PCI_REG_QUEUE_NOTIFY    0x10
#define VIRTIO_PCI_REG_DEVICE_STATUS   0x12
#define VIRTIO_PCI_REG_ISR_STATUS      0x13
#define VIRTIO_PCI_REG_CONFIG          0x14

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04
#define VIRTIO_STATUS_FAILED      0x80

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#define VIRTQ_ALIGN        4096U
#define VIRTIO_RX_QUEUE    0
#define VIRTIO_TX_QUEUE    1
#define VIRTIO_RX_SLOTS    8
#define VIRTIO_TX_SLOT     0
#define VIRTIO_FRAME_MAX   1514
#define VIRTIO_RX_BUF_SIZE 2048

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

struct virtio_queue {
    uint16_t queue_size;
    uint16_t active_descs;
    uint16_t last_used_idx;
    uint64_t ring_phys;
    uint8_t* ring_virt;
    struct vring_desc* desc;
    struct vring_avail* avail;
    struct vring_used* used;
};

static struct virtio_queue g_rxq;
static struct virtio_queue g_txq;
static uint8_t* g_rx_bufs[VIRTIO_RX_SLOTS];
static uint64_t g_rx_buf_phys[VIRTIO_RX_SLOTS];
static uint8_t* g_tx_buf;
static uint64_t g_tx_buf_phys;
static uint16_t g_iobase;
static uint8_t g_mac[6];
static int g_ready = 0;
static int g_tx_busy = 0;
static virtio_net_rx_cb_t g_rx_cb = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void* kernel_memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

static uint32_t virtq_bytes(uint16_t queue_size) {
    uint32_t avail_bytes = (uint32_t)(sizeof(uint16_t) * (3U + queue_size));
    uint32_t used_off = (uint32_t)((sizeof(struct vring_desc) * queue_size + avail_bytes + (VIRTQ_ALIGN - 1)) & ~(VIRTQ_ALIGN - 1));
    uint32_t used_bytes = (uint32_t)(sizeof(uint16_t) * 3U + sizeof(struct vring_used_elem) * queue_size);
    return used_off + used_bytes;
}

static void virtq_layout(struct virtio_queue* q) {
    uint32_t avail_bytes = (uint32_t)(sizeof(uint16_t) * (3U + q->queue_size));
    uint32_t used_off = (uint32_t)((sizeof(struct vring_desc) * q->queue_size + avail_bytes + (VIRTQ_ALIGN - 1)) & ~(VIRTQ_ALIGN - 1));
    q->desc = (struct vring_desc*)q->ring_virt;
    q->avail = (struct vring_avail*)(q->ring_virt + sizeof(struct vring_desc) * q->queue_size);
    q->used = (struct vring_used*)(q->ring_virt + used_off);
}

static int virtq_init(uint16_t queue_index, struct virtio_queue* q) {
    if (!q) return -1;

    outw((uint16_t)(g_iobase + VIRTIO_PCI_REG_QUEUE_SEL), queue_index);
    q->queue_size = inw((uint16_t)(g_iobase + VIRTIO_PCI_REG_QUEUE_NUM));
    if (q->queue_size == 0) return -1;

    uint32_t bytes = virtq_bytes(q->queue_size);
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    void* ring_phys = pmm_alloc(pages);
    if (!ring_phys) return -1;

    q->ring_phys = (uint64_t)ring_phys;
    q->ring_virt = (uint8_t*)PHYS_TO_VIRT(ring_phys);
    q->active_descs = 0;
    q->last_used_idx = 0;
    kernel_memset(q->ring_virt, 0, pages * PAGE_SIZE);
    virtq_layout(q);

    outl((uint16_t)(g_iobase + VIRTIO_PCI_REG_QUEUE_PFN), (uint32_t)(q->ring_phys / PAGE_SIZE));
    return 0;
}

static void puthex8(uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    char s[3];
    s[0] = hex[(v >> 4) & 0xF];
    s[1] = hex[v & 0xF];
    s[2] = '\0';
    puts(s);
}

static void virtio_net_fail(const char* msg) {
    outb((uint16_t)(g_iobase + VIRTIO_PCI_REG_DEVICE_STATUS), VIRTIO_STATUS_FAILED);
    puts("[net] virtio-net init failed: ");
    puts(msg);
    puts("\r\n");
}

static void virtio_net_reclaim_tx(void) {
    while (g_txq.last_used_idx != g_txq.used->idx) {
        g_txq.last_used_idx++;
        g_tx_busy = 0;
    }
}

static void virtio_net_kick(uint16_t queue_index) {
    __sync_synchronize();
    outw((uint16_t)(g_iobase + VIRTIO_PCI_REG_QUEUE_NOTIFY), queue_index);
}

int virtio_net_init(void) {
    if (g_ready) return 0;

    struct pci_device_info dev;
    if (pci_find_virtio_net(&dev) < 0) {
        return -1;
    }
    if (dev.vendor_id != VIRTIO_PCI_VENDOR_ID ||
        dev.device_id < VIRTIO_PCI_DEVICE_NET_MIN ||
        dev.device_id > VIRTIO_PCI_DEVICE_NET_MAX) {
        return -1;
    }

    g_iobase = pci_get_bar0_iobase(&dev);
    if (g_iobase == 0) {
        return -1;
    }

    pci_enable_io_busmaster(&dev);

    outb((uint16_t)(g_iobase + VIRTIO_PCI_REG_DEVICE_STATUS), 0);
    outb((uint16_t)(g_iobase + VIRTIO_PCI_REG_DEVICE_STATUS), VIRTIO_STATUS_ACKNOWLEDGE);
    outb((uint16_t)(g_iobase + VIRTIO_PCI_REG_DEVICE_STATUS), VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    (void)inl((uint16_t)(g_iobase + VIRTIO_PCI_REG_DEVICE_FEATURES));
    outl((uint16_t)(g_iobase + VIRTIO_PCI_REG_GUEST_FEATURES), 0);

    if (virtq_init(VIRTIO_RX_QUEUE, &g_rxq) < 0) {
        virtio_net_fail("rx queue");
        return -1;
    }
    if (virtq_init(VIRTIO_TX_QUEUE, &g_txq) < 0) {
        virtio_net_fail("tx queue");
        return -1;
    }

    if (g_rxq.queue_size > VIRTIO_RX_SLOTS) {
        g_rxq.active_descs = VIRTIO_RX_SLOTS;
    } else {
        g_rxq.active_descs = g_rxq.queue_size;
    }
    g_txq.active_descs = 1;

    for (uint16_t i = 0; i < g_rxq.active_descs; i++) {
        void* buf_phys = pmm_alloc(1);
        if (!buf_phys) {
            virtio_net_fail("rx buffers");
            return -1;
        }
        g_rx_buf_phys[i] = (uint64_t)buf_phys;
        g_rx_bufs[i] = (uint8_t*)PHYS_TO_VIRT(buf_phys);
        kernel_memset(g_rx_bufs[i], 0, PAGE_SIZE);

        g_rxq.desc[i].addr = g_rx_buf_phys[i];
        g_rxq.desc[i].len = sizeof(struct virtio_net_hdr) + VIRTIO_RX_BUF_SIZE;
        g_rxq.desc[i].flags = VRING_DESC_F_WRITE;
        g_rxq.desc[i].next = 0;
        g_rxq.avail->ring[i] = i;
    }
    g_rxq.avail->idx = g_rxq.active_descs;

    void* tx_phys = pmm_alloc(1);
    if (!tx_phys) {
        virtio_net_fail("tx buffer");
        return -1;
    }
    g_tx_buf_phys = (uint64_t)tx_phys;
    g_tx_buf = (uint8_t*)PHYS_TO_VIRT(tx_phys);
    kernel_memset(g_tx_buf, 0, PAGE_SIZE);
    g_txq.desc[VIRTIO_TX_SLOT].addr = g_tx_buf_phys;
    g_txq.desc[VIRTIO_TX_SLOT].len = 0;
    g_txq.desc[VIRTIO_TX_SLOT].flags = 0;
    g_txq.desc[VIRTIO_TX_SLOT].next = 0;

    for (uint16_t i = 0; i < 6; i++) {
        g_mac[i] = inb((uint16_t)(g_iobase + VIRTIO_PCI_REG_CONFIG + i));
    }

    virtio_net_kick(VIRTIO_RX_QUEUE);
    outb((uint16_t)(g_iobase + VIRTIO_PCI_REG_DEVICE_STATUS),
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    g_ready = 1;
    puts("[net] virtio-net ready mac=");
    for (uint16_t i = 0; i < 6; i++) {
        puthex8(g_mac[i]);
        if (i + 1 != 6) puts(":");
    }
    puts(" io=0x");
    puthex(g_iobase);
    puts("\r\n");
    return 0;
}

void virtio_net_poll(void) {
    if (!g_ready) return;

    virtio_net_reclaim_tx();

    uint16_t requeued = 0;
    while (g_rxq.last_used_idx != g_rxq.used->idx) {
        struct vring_used_elem* elem = &g_rxq.used->ring[g_rxq.last_used_idx % g_rxq.queue_size];
        uint16_t desc_id = (uint16_t)elem->id;
        uint32_t total_len = elem->len;
        uint16_t frame_len = 0;
        if (desc_id < g_rxq.active_descs && total_len > sizeof(struct virtio_net_hdr)) {
            frame_len = (uint16_t)(total_len - sizeof(struct virtio_net_hdr));
            if (frame_len > VIRTIO_RX_BUF_SIZE) frame_len = VIRTIO_RX_BUF_SIZE;
            if (g_rx_cb && frame_len > 0) {
                g_rx_cb(g_rx_bufs[desc_id] + sizeof(struct virtio_net_hdr), frame_len);
            }
            g_rxq.avail->ring[g_rxq.avail->idx % g_rxq.queue_size] = desc_id;
            g_rxq.avail->idx++;
            requeued++;
        }
        g_rxq.last_used_idx++;
    }

    if (requeued) {
        virtio_net_kick(VIRTIO_RX_QUEUE);
    }

    (void)inb((uint16_t)(g_iobase + VIRTIO_PCI_REG_ISR_STATUS));
}

int virtio_net_is_ready(void) {
    return g_ready;
}

int virtio_net_send(const void* frame, uint16_t len) {
    if (!g_ready || !frame || len == 0 || len > VIRTIO_FRAME_MAX) return -1;

    virtio_net_reclaim_tx();
    if (g_tx_busy) return -1;

    struct virtio_net_hdr* hdr = (struct virtio_net_hdr*)g_tx_buf;
    uint8_t* payload = g_tx_buf + sizeof(struct virtio_net_hdr);
    const uint8_t* src = (const uint8_t*)frame;

    kernel_memset(hdr, 0, sizeof(*hdr));
    for (uint16_t i = 0; i < len; i++) {
        payload[i] = src[i];
    }

    g_txq.desc[VIRTIO_TX_SLOT].len = (uint32_t)(sizeof(struct virtio_net_hdr) + len);
    g_txq.avail->ring[g_txq.avail->idx % g_txq.queue_size] = VIRTIO_TX_SLOT;
    g_txq.avail->idx++;
    g_tx_busy = 1;
    virtio_net_kick(VIRTIO_TX_QUEUE);
    return 0;
}

const uint8_t* virtio_net_mac(void) {
    return g_mac;
}

void virtio_net_set_rx_callback(virtio_net_rx_cb_t cb) {
    g_rx_cb = cb;
}
