# Orthox-64

![Orthox-64 Desktop](assets/screenshot.png)
![Orthox-64 DOOM](assets/doom.png)

Orthox-64 is a project that presents a modern approach to operating system development from scratch.

## Concept and Design Philosophy
- **Etymology:** "Ortho-" comes from orthogonality and correctness, combined with "-x" from the Unix tradition. Orthox-64 aims to be an orthodox, minimal Unix-like operating system.
- **Lightweight & Robust:** Orthox-64 rejects unnecessary system complexity and focuses on a small, stable kernel and userland substrate.
- **Pragmatic Unix Compatibility:** Rather than pursuing full POSIX compliance, it implements the minimum kernel and libc surface needed to run practical software such as BusyBox, GCC, and related toolchain components.
- **Integration over Reinvention:** Orthox-64 does not treat reimplementing every component from scratch as a virtue. Instead, it treats the integration and porting of high-quality existing open-source software as a primary engineering discipline.

## Features
- **64-bit Long Mode:** Runs in full 64-bit mode.
- **Bootloader:** Uses [Limine](https://github.com/limine-bootloader/limine) for modern UEFI/BIOS booting.
- **Memory Management:** PMM (Physical Memory Manager) and VMM (Virtual Memory Manager) with paging.
- **Multitasking:** Preemptive multitasking, kernel threads, and an SMP-ready per-CPU scheduler base.
- **File System:** Virtual File System (VFS), RetroFS-based root filesystem support, and tar-based initial ramdisk tooling for bring-up and fallback workflows.
- **USB Support:** Basic USB stack and Mass Storage Class (MSC) support.
- **Networking:** `virtio-net` + `lwIP` based IPv4 networking with DHCP, DNS, ICMP, UDP, TCP, socket syscalls, BusyBox `httpd`, outbound HTTP client support, and userland HTTPS client support with BearSSL.
- **SMP:** 4 CPU bring-up, LAPIC timer, reschedule IPI, per-CPU run queue, and validated blocking wakeup paths for pipe, wait, and socket workloads.
- **Sound:** Audio support via Intel HD Audio.
- **Userland:** Environment based on `musl libc` for better standard compatibility.
- **Ported Apps:** Capable of running ported software like `doomgeneric`.

## Ported Userland Components
- **musl libc:** `1.2.5`
- **BusyBox (`ash` and core applets):** `1.27.0.git`
- **GNU Binutils:** `2.26`
- **GCC:** `4.7.4`
- **doomgeneric:** Vendored local port, upstream version not recorded in-tree

## Status
The project is currently in active development. The current SMP scheduler phase has reached its acceptance target: Orthox-64 now boots and runs on 4 CPUs in QEMU, uses a per-CPU run queue, restores fork spread, and has validated pipe, `wait4()`, DNS, socket, UDP echo, BusyBox `httpd`, and userspace HTTPS paths under SMP. Current focus is on expanding userland compatibility and moving from this stabilized SMP base toward broader load balancing and higher-level functionality.

## Acknowledgements
Orthox-64 is inspired by and references the following projects:
- **[MikanOS](https://github.com/uchan-nos/mikanos)**: A modern educational OS by [uchan-nos](https://github.com/uchan-nos). The kernel architecture and some primitive setups were developed with reference to its implementation.
- **[Limine](https://github.com/limine-bootloader/limine)**: Used as the bootloader for modern UEFI/BIOS support.
- **[retro-rocket](https://github.com/brainboxdotcc/retro-rocket)**: Referenced while integrating RetroFS support into Orthox-64.
