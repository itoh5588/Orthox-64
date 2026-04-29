#include "wait.h"

static int wait_queue_contains_locked(struct wait_queue* q, struct task* t) {
    struct task* it = q ? q->head : 0;
    while (it) {
        if (it == t) return 1;
        it = it->wait_next;
    }
    return 0;
}

static void wait_queue_add_current_locked(struct wait_queue* q, struct task* current) {
    if (!q || !current) return;
    if (wait_queue_contains_locked(q, current)) return;
    current->wait_queue = q;
    current->wait_next = q->head;
    q->head = current;
}

static struct task* wait_queue_pop_locked(struct wait_queue* q) {
    struct task* t;
    if (!q || !q->head) return 0;
    t = q->head;
    q->head = t->wait_next;
    t->wait_next = 0;
    t->wait_queue = 0;
    return t;
}

void wait_queue_init(struct wait_queue* q) {
    if (!q) return;
    spinlock_init(&q->lock);
    q->head = 0;
}

int wait_queue_empty(struct wait_queue* q) {
    int empty;
    uint64_t flags;
    if (!q) return 1;
    flags = spin_lock_irqsave(&q->lock);
    empty = q->head == 0;
    spin_unlock_irqrestore(&q->lock, flags);
    return empty;
}

int wait_event(struct wait_queue* q, wait_condition_t condition, void* arg) {
    struct task* current;
    if (!q || !condition) return -1;
    current = get_current_task();
    if (!current) return -1;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&q->lock);
        if (condition(arg)) {
            spin_unlock_irqrestore(&q->lock, flags);
            return 0;
        }
        wait_queue_add_current_locked(q, current);
        task_mark_io_wait(current);
        spin_unlock_irqrestore(&q->lock, flags);
        kernel_yield();
    }
}

void wake_up_one(struct wait_queue* q) {
    struct task* t;
    uint64_t flags;
    if (!q) return;
    flags = spin_lock_irqsave(&q->lock);
    t = wait_queue_pop_locked(q);
    spin_unlock_irqrestore(&q->lock, flags);
    if (t) task_wake(t);
}

void wake_up_all(struct wait_queue* q) {
    if (!q) return;
    for (;;) {
        struct task* t;
        uint64_t flags = spin_lock_irqsave(&q->lock);
        t = wait_queue_pop_locked(q);
        spin_unlock_irqrestore(&q->lock, flags);
        if (!t) break;
        task_wake(t);
    }
}

static int completion_done(void* arg) {
    struct completion* c = (struct completion*)arg;
    return c && c->done > 0;
}

void init_completion(struct completion* c) {
    if (!c) return;
    c->done = 0;
    c->status = 0;
    wait_queue_init(&c->wait);
}

void reinit_completion(struct completion* c) {
    if (!c) return;
    c->done = 0;
    c->status = 0;
}

void complete(struct completion* c) {
    complete_status(c, 0);
}

void complete_status(struct completion* c, int status) {
    if (!c) return;
    {
        uint64_t flags = spin_lock_irqsave(&c->wait.lock);
        c->status = status;
        c->done++;
        spin_unlock_irqrestore(&c->wait.lock, flags);
    }
    wake_up_one(&c->wait);
}

void complete_all(struct completion* c) {
    complete_all_status(c, 0);
}

void complete_all_status(struct completion* c, int status) {
    if (!c) return;
    {
        uint64_t flags = spin_lock_irqsave(&c->wait.lock);
        c->status = status;
        c->done = 0x7fffffff;
        spin_unlock_irqrestore(&c->wait.lock, flags);
    }
    wake_up_all(&c->wait);
}

void wait_for_completion(struct completion* c) {
    (void)wait_for_completion_status(c);
}

int wait_for_completion_status(struct completion* c) {
    int status;
    if (!c) return -1;
    if (wait_event(&c->wait, completion_done, c) < 0) return -1;
    {
        uint64_t flags = spin_lock_irqsave(&c->wait.lock);
        if (c->done > 0) c->done--;
        status = c->status;
        spin_unlock_irqrestore(&c->wait.lock, flags);
    }
    return status;
}
