#include <stddef.h>
#include <stdint.h>
#include "fs.h"
#include "pmm.h"
#include "riscv64/bootstrap_user.h"
#include "task.h"
#include "vmm.h"

int fs_get_file_data(const char* path, void** data, size_t* size) {
    if (riscv64_bootstrap_user_file_data(path, data, size) == 0) return 0;
    if (data) *data = 0;
    if (size) *size = 0;
    return -1;
}

int sys_close(int fd) {
    struct task* current = get_current_task();
    file_descriptor_t* desc;
    struct task* read_waiter = 0;
    struct task* write_waiter = 0;
    int free_pipe = 0;

    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS) return -1;
    desc = &current->fds[fd];
    if (!desc->in_use) return -1;

    if (desc->type == FT_PIPE) {
        pipe_t* pipe = (pipe_t*)desc->data;
        if (pipe) {
            uint64_t flags = spin_lock_irqsave(&pipe->lock);
            if (pipe->ref_count > 0) pipe->ref_count--;
            read_waiter = pipe->read_waiter;
            write_waiter = pipe->write_waiter;
            pipe->read_waiter = 0;
            pipe->write_waiter = 0;
            free_pipe = (pipe->ref_count == 0);
            spin_unlock_irqrestore(&pipe->lock, flags);
            if (read_waiter && read_waiter->state == TASK_SLEEPING) task_wake(read_waiter);
            if (write_waiter && write_waiter->state == TASK_SLEEPING) task_wake(write_waiter);
            if (free_pipe) {
                pmm_free((void*)VIRT_TO_PHYS((uint64_t)pipe), 1);
            }
        }
    } else if (desc->type == FT_DIR) {
        if (desc->data) {
            pmm_free((void*)VIRT_TO_PHYS((uint64_t)desc->data), (size_t)desc->aux0);
        }
    }

    desc->in_use = 0;
    desc->data = 0;
    desc->size = 0;
    desc->offset = 0;
    desc->name[0] = '\0';
    return 0;
}
