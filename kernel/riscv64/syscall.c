#include <stdint.h>
#include "riscv64/boot.h"
#include "spinlock.h"
#include "riscv64/syscall.h"
#include "syscall.h"
#include "task.h"

static task_context_t* g_riscv64_fallback_current_context;

static void riscv64_bootstrap_sys_exit(int status) {
    (void)status;
    riscv64_uart_puts("  bootstrap user exit\n");
    riscv64_wait_forever();
}

static void riscv64_bootstrap_syscall_dispatch(arch_syscall_frame_t* frame) {
    if (!frame) return;

    switch (arch_syscall_number(frame)) {
        case SYS_WRITE:
            {
                const char* src = (const char*)(uintptr_t)arch_syscall_arg1(frame);
                size_t len = (size_t)arch_syscall_arg2(frame);
                int fd = (int)arch_syscall_arg0(frame);
                if (fd != 1 && fd != 2) {
                    arch_syscall_set_return(frame, (uint64_t)-1);
                    return;
                }
                for (size_t i = 0; i < len; i++) {
                    riscv64_uart_putchar(src[i]);
                }
                arch_syscall_set_return(frame, len);
                return;
            }
        case SYS_GETCWD:
            {
                struct task* task = get_current_task();
                char* dst = (char*)(uintptr_t)arch_syscall_arg0(frame);
                size_t dst_size = (size_t)arch_syscall_arg1(frame);
                const char* cwd = (task && task->cwd[0]) ? task->cwd : "/";
                size_t i = 0;
                if (!dst || dst_size == 0) {
                    arch_syscall_set_return(frame, 0);
                    return;
                }
                while (cwd[i] && i + 1 < dst_size) {
                    dst[i] = cwd[i];
                    i++;
                }
                if (cwd[i] != '\0' && i + 1 >= dst_size) {
                    arch_syscall_set_return(frame, 0);
                    return;
                }
                dst[i] = '\0';
                arch_syscall_set_return(frame, (uint64_t)(uintptr_t)dst);
                return;
            }
        case SYS_GETPID:
            {
                struct task* current = get_current_task();
                arch_syscall_set_return(frame, current ? (uint64_t)current->pid : 0);
                return;
            }
        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            riscv64_bootstrap_sys_exit((int)arch_syscall_arg0(frame));
            return;
        default:
            arch_syscall_set_return(frame, (uint64_t)-38);
            return;
    }
}

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

    riscv64_bootstrap_syscall_dispatch(&syscall_frame);

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
