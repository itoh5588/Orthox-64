#ifndef ORTHOX_ARCH_RISCV64_SYSCALL_H
#define ORTHOX_ARCH_RISCV64_SYSCALL_H

#include "riscv64/trap.h"

struct arch_task_context;

void riscv64_syscall_dispatch(riscv64_trap_frame_t* frame);
void riscv64_syscall_sync_current_user_frame(const riscv64_trap_frame_t* frame);
void riscv64_syscall_set_current_context(struct arch_task_context* ctx);

#endif
