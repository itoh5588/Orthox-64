# Orthox-64 Kernel Source Structure

This directory contains the core source code for the Orthox-64 kernel.

## File Breakdown

### Core and Initialization
- **`init.c`**: Kernel entry point and early bring-up sequence. Receives control from Limine, initializes memory management, descriptor tables, interrupts, syscall entry, tasking, PCI, networking, USB, and starts the first user-space process.
- **`elf.c`**: ELF loader used by both the initial task bring-up path and later `execve()` style process loading.

### Memory Management
- **`pmm.c`**: Physical Memory Manager. Tracks free physical pages and provides page-granular allocation and free services.
- **`vmm.c`**: Virtual Memory Manager. Builds and updates x86_64 4-level page tables, maps user/kernel memory, and switches address spaces.

### CPU Setup and Interrupts
- **`gdt.c` / `gdt_flush.S`**: GDT and TSS setup for kernel/user privilege transitions.
- **`idt.c` / `interrupt.S`**: IDT setup and interrupt/exception entry stubs.
- **`lapic.c`**: Local APIC timer and timing support used by scheduling and `lwIP` timeouts.
- **`pic.c`**: Legacy 8259 PIC masking/unmasking for IRQ routing compatibility.

### Tasking and Process Control
- **`task.c` / `task_switch.S`**: Preemptive task scheduler, task creation, process context, fork/exec helpers, and low-level context switching.
- **`syscall.c` / `syscall_entry.S`**: `syscall` entry path and dispatcher for file, process, tty, signal, network, USB, and Orthox-64 private syscalls.

### File System and Console I/O
- **`fs.c`**: VFS layer, file descriptor table handling, tar-backed root file system, directory traversal, tty/console plumbing, and generic `read`/`write` dispatch.
- **`keyboard.c`**: PS/2 keyboard and serial-input path feeding the shell's console input buffer.

### PCI, USB, and Devices
- **`pci.c`**: PCI enumeration and discovery of devices such as `virtio-net`, audio, and xHCI controllers.
- **`usb.c`**: USB/xHCI and mass-storage-oriented code used for USB storage access and rootfs mounting experiments.
- **`sound.c`**: Intel HD Audio / PCM playback support.

### Networking
- **`virtio_net.c`**: Minimal `virtio-net` driver using legacy virtio-pci I/O BARs, RX/TX virtqueues, MAC discovery, and polling-based frame I/O.
- **`net.c`**: Thin NIC abstraction layer that sits between the driver and upper networking code. Exposes frame send, MAC lookup, RX callback registration, and polling.
- **`lwip_port.c`**: `lwIP` integration layer (`NO_SYS=1`). Brings up the netif, runs DHCP/DNS/timeout processing, handles ARP/ICMP diagnostics, UDP echo, and kernel-side DNS lookup glue.
- **`net_socket.c`**: Kernel socket backend on top of `lwIP`. Implements the current `AF_INET` socket path for UDP and TCP, including bind/connect/listen/accept/send/recv and fd integration.

### Freestanding libc Fragments
- **`cstring.c`**: Minimal string and memory routines required by the kernel and vendored components such as `lwIP`.
- **`cstdio.c`**: Minimal formatted output helpers and serial-oriented stdio support used inside the kernel.
- **`cstdlib.c`**: Minimal libc-style utility functions needed by freestanding kernel code.

## Notes
- **`idt.c.bak_doom_sched_test`**: Backup file kept from earlier scheduler experiments during the DOOM bring-up phase.
