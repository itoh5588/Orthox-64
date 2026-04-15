.section .text
.global _start
.extern environ
.extern _tls_init

_start:
    # Save argc, argv, envp from registers (Kernel passes them in rdi, rsi, rdx)
    # Save envp to global environ for Newlib
    mov %rdx, environ(%rip)

    # Find Auxiliary Vector on the stack
    # Stack layout: argc, argv..., NULL, envp..., NULL, auxv...
    # But we already have envp in %rdx.
    # Auxv starts right after the NULL that terminates envp.
    mov %rdx, %rax
.find_auxv:
    cmpq $0, (%rax)
    lea 8(%rax), %rax
    jne .find_auxv
    # %rax now points to the first entry of Auxv
    
    # Call _tls_init(auxv)
    mov %rax, %rdi
    call _tls_init

    # SysV x86_64 ABI: align stack before calling into C code.
    # Note: argc, argv, envp are still in rdi, rsi, rdx from the kernel (if main needs them)
    # Wait, _tls_init might have clobbered them. Let's restore or use the ones from the stack.
    # Since we need to call main(argc, argv, envp), let's get them from stack.
    mov 0(%rsp), %rdi    # argc
    lea 8(%rsp), %rsi    # argv
    # envp is after argv
    mov %rdi, %rdx
    lea 8(%rsi, %rdx, 8), %rdx # envp

    andq $-16, %rsp

    # Call main(argc, argv, envp)
    call main

    # Call _exit(rax)
    mov %rax, %rdi
    call _exit

.loop:
    hlt
    jmp .loop
