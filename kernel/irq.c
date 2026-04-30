#include "irq.h"
#include "spinlock.h"

struct irq_slot {
    irq_handler_t handler;
    void* ctx;
};

static spinlock_t g_irq_lock;
static struct irq_slot g_legacy_irq_slots[16];

int irq_register_legacy(int irq, irq_handler_t handler, void* ctx) {
    int ret = 0;
    uint64_t flags;
    if (irq < 0 || irq >= 16 || !handler) return -1;

    flags = spin_lock_irqsave(&g_irq_lock);
    if (g_legacy_irq_slots[irq].handler) {
        ret = -1;
    } else {
        g_legacy_irq_slots[irq].handler = handler;
        g_legacy_irq_slots[irq].ctx = ctx;
    }
    spin_unlock_irqrestore(&g_irq_lock, flags);
    return ret;
}

int irq_dispatch_legacy(int irq) {
    irq_handler_t handler;
    void* ctx;
    uint64_t flags;

    if (irq < 0 || irq >= 16) return 0;

    flags = spin_lock_irqsave(&g_irq_lock);
    handler = g_legacy_irq_slots[irq].handler;
    ctx = g_legacy_irq_slots[irq].ctx;
    spin_unlock_irqrestore(&g_irq_lock, flags);

    if (!handler) return 0;
    return handler(irq, ctx);
}
