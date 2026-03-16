# Orthox-64

![Orthox-64 Desktop](assets/screenshot.png)
![Orthox-64 DOOM](assets/doom.png)

Orthox-64 is a project that presents a modern approach to operating system development from scratch.

## Concept and Design Philosophy
- **Etymology:** "Orthogonality/Correctness (Ortho-)" + "Unix Tradition (-x)". Aiming for an "Orthodox" Unix-like OS.
- **Lightweight & Robust:** An antithesis to bloated modern operating systems, pursuing maximum stability with minimal code.
- **POSIX Subset:** Instead of chasing full POSIX compliance, it implements only the "POSIX Core" necessary for running essential software (e.g., BusyBox, GCC).
- **Beyond Reinventing the Wheel:** Avoids the "penance" of writing bootloaders, shells, TCP/IP stacks, and compilers from scratch. Instead, it emphasizes the engineering methodology of "integrating (porting)" existing high-quality open-source assets.

## Features
- **64-bit Long Mode:** Runs in full 64-bit mode.
- **Bootloader:** Uses [Limine](https://github.com/limine-bootloader/limine) for modern UEFI/BIOS booting.
- **Memory Management:** PMM (Physical Memory Manager) and VMM (Virtual Memory Manager) with paging.
- **Multitasking:** Preemptive multitasking and kernel threads.
- **File System:** Virtual File System (VFS) and tar-based initial ramdisk.
- **USB Support:** Basic USB stack and Mass Storage Class (MSC) support.
- **Sound:** Audio support via Intel HD Audio.
- **Userland:** Environment based on `musl libc` for better standard compatibility.
- **Ported Apps:** Capable of running ported software like `doomgeneric`.

## Status
The project is currently in active development. Core kernel primitives are stable, and current focus is on improving the userland environment and porting standard tools.

## Acknowledgements
Orthox-64 is inspired by and references the following projects:
- **[MikanOS](https://github.com/uchan-nos/mikanos)**: A modern educational OS by [uchan-nos](https://github.com/uchan-nos). The kernel architecture and some primitive setups were developed with reference to its implementation.
- **[Limine](https://github.com/limine-bootloader/limine)**: Used as the bootloader for modern UEFI/BIOS support.
