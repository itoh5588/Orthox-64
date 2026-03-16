# Orthox-64 Kernel Source Structure

This directory contains the core source code for the Orthox-64 kernel.

## File Breakdown and Detailed Descriptions

### Core & Initialization
- **`main.c`**: Kernel entry point. Receives control from the bootloader, initializes all major subsystems (PMM, VMM, GDT/IDT, Tasking, FS, etc.) in order, and kicks off the first user-space process.

### Memory Management
- **`pmm.c`**: Physical Memory Manager. Responsible for tracking available physical page frames (4KB units) and providing interfaces for allocation and deallocation of physical memory.
- **`vmm.c`**: Virtual Memory Manager. Manages the x86_64 4-level paging scheme. It handles virtual-to-physical address mappings, page table construction, and switching address spaces between processes.

### Segmentation & Interrupts
- **`gdt.c` / `gdt_flush.S`**: Global Descriptor Table setup. Defines code and data segments for both kernel and user-land, and configures the Task State Segment (TSS) for reliable stack switching during privilege level transitions.
- **`idt.c` / `interrupt.S`**: Interrupt Descriptor Table setup. Defines handlers for CPU exceptions (e.g., page faults) and external interrupts (e.g., timer, keyboard). The assembly wrapper (`interrupt.S`) manages register context saving and restoration.
- **`lapic.c` / `pic.c`**: Interrupt Controller management. Provides interfaces for both modern Local APIC (`lapic.c`) and legacy 8259A PIC (`pic.c`) to route interrupts to the appropriate devices.

### Multitasking
- **`task.c` / `task_switch.S`**: Implementation of preemptive multitasking. Manages process creation, scheduling, and time-slice logic. `task_switch.S` contains the low-level architecture-specific code for context switching.

### System Calls & Loader
- **`syscall.c` / `syscall_entry.S`**: System call interface via the `syscall` instruction. Dispatches user-space requests to kernel services like file I/O or memory management.
- **`elf.c`**: ELF executable loader. Parses ELF headers and loads program segments into memory, preparing the process for execution.

### File System & I/O
- **`fs.c`**: Virtual File System (VFS) abstraction. Manages file descriptors and provides support for reading from a tar-formatted initial RAM disk (initrd).
- **`pci.c`**: PCI bus enumeration. Scans the PCI bus to discover and identify connected hardware such as audio controllers and USB host controllers.

### Device Drivers
- **`keyboard.c`**: PS/2 Keyboard driver. Translates scan codes into ASCII and manages an input buffer for user interactions.
- **`usb.c`**: Basic USB stack. Implements host controller initialization and experimental support for USB Mass Storage Class (MSC) devices.
- **`sound.c`**: Intel High Definition Audio (HDA) driver. Supports PCM audio playback, enabling the OS to produce sound.

---

## Notes
- **`idt.c.bak_doom_sched_test`**: A backup file for scheduler testing during the DOOM porting phase.
