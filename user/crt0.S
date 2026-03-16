.section .text
.global _start
.extern environ

_start:
    # Kernel passes argc in rdi, argv in rsi, envp in rdx
    # Save envp to global environ for Newlib
    mov %rdx, environ(%rip)

    # SysV x86_64 ABI: align stack before calling into C code.
    andq $-16, %rsp

    # Call main(argc, argv, envp)
    call main

    # Call _exit(rax)
    mov %rax, %rdi
    call _exit

.loop:
    hlt
    jmp .loop
