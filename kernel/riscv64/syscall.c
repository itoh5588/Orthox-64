#include <stdint.h>
#include "riscv64/syscall.h"
#include "syscall.h"
#include "task.h"

static task_context_t* g_riscv64_fallback_current_context;

void riscv64_syscall_dispatch(riscv64_trap_frame_t* frame) {
    arch_syscall_frame_t syscall_frame;
    if (!frame) return;

    syscall_frame.r15 = 0;
    syscall_frame.r14 = 0;
    syscall_frame.r13 = 0;
    syscall_frame.r12 = 0;
    syscall_frame.rbp = frame->s0;
    syscall_frame.rbx = frame->s1;
    arch_syscall_set_arg5(&syscall_frame, frame->a5);
    arch_syscall_set_arg4(&syscall_frame, frame->a4);
    arch_syscall_set_arg3(&syscall_frame, frame->a3);
    arch_syscall_set_arg2(&syscall_frame, frame->a2);
    arch_syscall_set_arg1(&syscall_frame, frame->a1);
    arch_syscall_set_arg0(&syscall_frame, frame->a0);
    arch_syscall_set_number(&syscall_frame, frame->a7);
    arch_syscall_set_program_counter(&syscall_frame, frame->sepc + 4);
    syscall_frame.cs = 0;
    syscall_frame.rflags = frame->sstatus;
    arch_syscall_set_stack_pointer(&syscall_frame, frame->sp);
    syscall_frame.ss = 0;

    syscall_dispatch(&syscall_frame);

    frame->a3 = syscall_frame.r10;
    frame->a4 = syscall_frame.r8;
    frame->a5 = syscall_frame.r9;
    frame->s0 = syscall_frame.rbp;
    frame->s1 = syscall_frame.rbx;
    riscv64_trap_set_user_return(frame,
                                 arch_syscall_program_counter(&syscall_frame),
                                 arch_syscall_stack_pointer(&syscall_frame),
                                 arch_syscall_return(&syscall_frame),
                                 arch_syscall_arg1(&syscall_frame),
                                 arch_syscall_arg2(&syscall_frame));
    riscv64_syscall_sync_current_user_frame(frame);
}

void riscv64_syscall_sync_current_user_frame(const riscv64_trap_frame_t* frame) {
    task_context_t* ctx;
    if (!frame) return;
    ctx = task_current_context();
    if (!ctx) ctx = g_riscv64_fallback_current_context;
    if (!ctx) return;
    riscv64_task_store_user_frame(ctx, frame);
}

void riscv64_syscall_set_current_context(struct arch_task_context* ctx) {
    g_riscv64_fallback_current_context = (task_context_t*)ctx;
}
