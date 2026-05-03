# Orthox-64 RISC-V `virt` Porting Plan

## Goal

This note covers two things:

1. A staged plan for bringing Orthox-64 to RISC-V `virt` on QEMU.
2. An inventory of the current x86-specific code and what must be abstracted.

The target is not "Raspberry Pi style board bring-up", but a cleaner first non-x86 port:

- architecture: `riscv64`
- platform: QEMU `virt`
- firmware path: OpenSBI + DTB
- devices: UART + PLIC + CLINT/timer + virtio-mmio first

That choice keeps the first port focused on architecture and kernel structure, not board-specific firmware and SoC quirks.

## Recommended End State

Orthox-64 should eventually be split along two axes:

- `arch/`
  - CPU/MMU/exception/syscall/context-switch code
- `platform/`
  - boot protocol, interrupt controller, timer, UART, device discovery

For the first RISC-V port, a practical target layout would be:

```text
arch/
  x86_64/
  riscv64/
platform/
  pc/
  qemu-virt-riscv/
kernel/
  fs.c
  task.c
  syscall.c
  net_socket.c
  lwip_port.c
  ...
```

The kernel core should stay architecture-neutral where possible. The current scheduler, wait/wake paths, VFS, and most socket logic can largely stay in shared code.

## Staged Plan

### Phase 0: Preparation

Goal: make the existing x86 tree easier to port without changing behavior.

Tasks:

- Introduce explicit `arch` and `platform` interfaces.
- Stop letting generic kernel code directly include x86 headers where avoidable.
- Separate:
  - trap/interrupt entry
  - context switch
  - page-table operations
  - CPU-local install
  - timer/IPI hooks
  - UART/console hooks

Exit condition:

- x86 build still works.
- Shared code no longer directly depends on `gdt.h`, `idt.h`, `lapic.h`, `pci.h`, `limine.h` except in architecture/platform glue.

### Phase 1: Minimal RISC-V Boot To Serial

Goal: boot on QEMU `virt` and print to UART.

Tasks:

- Add `arch/riscv64` low-level start code.
- Boot via OpenSBI and obtain DTB pointer.
- Add SBI console fallback only if needed during very early bring-up.
- Initialize UART16550 at the `virt` machine address and get `puts()` working.

Exit condition:

- QEMU `virt` boots and prints a stable early banner.

### Phase 2: Trap Entry and Timer

Goal: enter the kernel on exceptions/interrupts and get periodic ticks.

Tasks:

- Implement RISC-V trap entry (`stvec`, save frame, restore frame, `sret`).
- Add supervisor timer interrupt handling.
- Use SBI timer first; later replace with direct timer path if needed.
- Replace x86 LAPIC tick dependency in scheduler-facing code with generic timer hooks.

Exit condition:

- periodic timer interrupts work
- `kernel_yield()` and timer-driven reschedule path can run on RISC-V

### Phase 3: RISC-V MMU and Address Space

Goal: reach the same conceptual level as current x86 paging on one CPU.

Tasks:

- Implement Sv39 page-table management.
- Add generic wrappers for:
  - map page/range
  - unmap page/range
  - activate address space
  - flush local TLB entry/range
- Port kernel higher-half or fixed virtual map policy to RISC-V.
- Load user ELF into a per-task page table.

Exit condition:

- user address spaces exist
- simple user task can be mapped and prepared

### Phase 4: Context Switch and Syscall Path

Goal: userland can enter and return from the kernel.

Tasks:

- Rework `task_context` into an architecture-specific saved register layout.
- Implement RISC-V `switch_context`.
- Implement RISC-V user entry path (`sret` instead of `iretq`).
- Implement syscall entry/dispatch using `ecall`.
- Define RISC-V syscall ABI glue for userland wrappers.

Exit condition:

- simple `/bin/sh` or a minimal user test can run on RISC-V `virt`

### Phase 5: Single-Core Shared Kernel Features

Goal: bring over the existing high-level Orthox-64 features on one CPU.

Tasks:

- reuse:
  - `fs.c`
  - `task.c`
  - `syscall.c`
  - `net_socket.c`
  - `lwip_port.c`
- keep framebuffer, sound, USB out of scope initially
- bring up:
  - shell
  - `fork/exec/wait`
  - `pipe`
  - lwIP with virtio networking

Exit condition:

- shell + file I/O + `fork/exec/wait` + `pipe` + network on single-core RISC-V

### Phase 6: Virtio-MMIO Devices

Goal: replace x86 PCI assumptions with QEMU `virt`-appropriate devices.

Tasks:

- port `virtio-net` from legacy virtio-pci I/O BAR model to virtio-mmio
- use DTB-based device discovery instead of PCI enumeration
- keep USB and sound disabled on RISC-V initially

Exit condition:

- `httpsfetch`, UDP echo, and a simple HTTP server work on RISC-V `virt`

### Phase 7: RISC-V SMP

Goal: restore the current Orthox-64 SMP model on RISC-V.

Tasks:

- CPU bring-up through SBI/HSM or the QEMU `virt` boot convention
- per-CPU state installation
- IPI/reschedule mechanism via RISC-V software interrupts
- timer per CPU
- reuse current:
  - per-CPU run queue
  - limited migration
  - wake/resched model

Exit condition:

- multi-hart bring-up
- scheduler and stress tests behave like current x86 SMP phase

## Suggested Implementation Order

Do not port everything at once. The right dependency order is:

1. `arch` / `platform` interface split on x86
2. RISC-V early boot + UART
3. trap + timer
4. MMU + task context
5. syscall + user entry
6. shell and core syscalls
7. virtio-mmio net
8. SMP

That order avoids mixing architecture bring-up with device-driver churn.

## x86 Dependency Inventory

### A. Boot Protocol and Platform Discovery

These are platform-specific and tied to the current PC/Limine environment.

- [`kernel/init.c`](/Users/itoh/Github-Orthox-64/kernel/init.c)
  - uses Limine requests:
    - memmap
    - HHDM
    - modules
    - kernel address
    - framebuffer
    - SMP
- [`kernel/elf.c`](/Users/itoh/Github-Orthox-64/kernel/elf.c)
  - includes `limine.h`
- [`kernel/vmm.c`](/Users/itoh/Github-Orthox-64/kernel/vmm.c)
  - uses Limine HHDM and kernel-address response
- [`iso/limine.conf`](/Users/itoh/Github-Orthox-64/iso/limine.conf)
  - current boot path is Limine-specific

RISC-V replacement:

- OpenSBI + DTB
- platform memory map from DTB / firmware handoff
- explicit kernel load convention for QEMU `virt`

### B. x86 Privilege Transition and Trap Machinery

These are architecture-specific and cannot be reused directly.

- [`kernel/x86_64/syscall_entry.S`](/Users/itoh/Github-Orthox-64/kernel/x86_64/syscall_entry.S)
  - `swapgs`
  - `iretq`
  - x86 syscall frame layout
- [`kernel/x86_64/interrupt.S`](/Users/itoh/Github-Orthox-64/kernel/x86_64/interrupt.S)
  - x86 interrupt stubs and `iretq`
- [`kernel/x86_64/task_switch.S`](/Users/itoh/Github-Orthox-64/kernel/x86_64/task_switch.S)
  - x86 register save/restore
  - `cr3`
  - `swapgs`
  - `iretq`
- [`kernel/x86_64/gdt.c`](/Users/itoh/Github-Orthox-64/kernel/x86_64/gdt.c)
- [`kernel/x86_64/gdt_flush.S`](/Users/itoh/Github-Orthox-64/kernel/x86_64/gdt_flush.S)
- [`kernel/x86_64/idt.c`](/Users/itoh/Github-Orthox-64/kernel/x86_64/idt.c)
- [`include/gdt.h`](/Users/itoh/Github-Orthox-64/include/gdt.h)
- [`include/idt.h`](/Users/itoh/Github-Orthox-64/include/idt.h)

RISC-V replacement:

- `stvec`, trap frame save/restore
- `sstatus`, `sepc`, `scause`, `stval`
- `sret`
- no GDT/TSS/`swapgs`

### C. x86 MMU and TLB Operations

These are architecture-specific but conceptually portable.

- [`kernel/vmm.c`](/Users/itoh/Github-Orthox-64/kernel/vmm.c)
  - x86 4-level page table assumptions
  - `mov %cr3`
  - `invlpg`
- [`kernel/x86_64/task_switch.S`](/Users/itoh/Github-Orthox-64/kernel/x86_64/task_switch.S)
  - task context stores `cr3`
- [`include/vmm.h`](/Users/itoh/Github-Orthox-64/include/vmm.h)
  - likely currently shaped around x86 paging
- [`include/task.h`](/Users/itoh/Github-Orthox-64/include/task.h)
  - `task_context` embeds `cr3`, `cs`, `ss`, `fs`, `gs`, x86 GPR set, `fxsave_area`

RISC-V replacement:

- Sv39 page tables
- `satp`
- `sfence.vma`
- architecture-specific `task_context`

This is one of the biggest structural changes needed.

### D. x86 CPU-Local and MSR Usage

These are architecture-specific.

- [`kernel/x86_64/syscall_entry.S`](/Users/itoh/Github-Orthox-64/kernel/x86_64/syscall_entry.S)
  - `swapgs`
- [`kernel/x86_64/task_switch.S`](/Users/itoh/Github-Orthox-64/kernel/x86_64/task_switch.S)
  - `swapgs`
- [`kernel/x86_64/idt.c`](/Users/itoh/Github-Orthox-64/kernel/x86_64/idt.c)
  - reads `MSR_GS_BASE` / `MSR_KERNEL_GS_BASE`
- [`kernel/vmm.c`](/Users/itoh/Github-Orthox-64/kernel/vmm.c)
  - debug reads of GS MSRs

RISC-V replacement:

- `tp` register for per-CPU / per-thread pointer
- explicit trap entry conventions instead of GS swaps

### E. Timer, IPI, and SMP Hardware

Current implementation is heavily PC/x86-specific.

- [`kernel/x86_64/lapic.c`](/Users/itoh/Github-Orthox-64/kernel/x86_64/lapic.c)
  - Local APIC timer
  - APIC IPI send
  - PIT-assisted calibration
- [`kernel/smp.c`](/Users/itoh/Github-Orthox-64/kernel/smp.c)
  - Limine SMP response
  - LAPIC IDs
- [`include/lapic.h`](/Users/itoh/Github-Orthox-64/include/lapic.h)
- [`include/smp.h`](/Users/itoh/Github-Orthox-64/include/smp.h)
- [`kernel/x86_64/pic.c`](/Users/itoh/Github-Orthox-64/kernel/x86_64/pic.c)
  - legacy 8259 PIC

RISC-V replacement:

- supervisor timer interrupts
- software interrupts for resched IPI
- hart IDs instead of LAPIC IDs
- SBI / platform startup path for secondary harts

The current scheduler logic in [`kernel/task.c`](/Users/itoh/Github-Orthox-64/kernel/task.c) is mostly reusable, but the low-level wakeup mechanism is not.

### F. PC Serial, Sound, and Legacy Port I/O

These are platform-specific, not just x86-specific.

- [`kernel/init.c`](/Users/itoh/Github-Orthox-64/kernel/init.c)
  - COM1 serial via `outb(0x3f8, ...)`
- [`kernel/sound.c`](/Users/itoh/Github-Orthox-64/kernel/sound.c)
  - PC speaker / SB16 / DMA / PIT / port I/O
- [`kernel/x86_64/lapic.c`](/Users/itoh/Github-Orthox-64/kernel/x86_64/lapic.c)
  - PIT calibration and port I/O

RISC-V `virt` replacement:

- UART16550 MMIO
- no sound initially

### G. PCI and Current Device Discovery

This is platform-specific and must be redesigned for `virt`.

- [`kernel/pci.c`](/Users/itoh/Github-Orthox-64/kernel/pci.c)
- [`include/pci.h`](/Users/itoh/Github-Orthox-64/include/pci.h)
- [`kernel/virtio_net.c`](/Users/itoh/Github-Orthox-64/kernel/virtio_net.c)
  - legacy virtio-pci
  - I/O BAR access
- [`include/virtio_net.h`](/Users/itoh/Github-Orthox-64/include/virtio_net.h)
- [`kernel/usb.c`](/Users/itoh/Github-Orthox-64/kernel/usb.c)
  - discovered through PCI/xHCI
- [`include/usb.h`](/Users/itoh/Github-Orthox-64/include/usb.h)

RISC-V `virt` replacement:

- DTB-based device discovery
- virtio-mmio for networking
- skip USB first

### H. Mostly Portable Kernel Core

These are the strongest candidates for reuse with only minor interface changes.

- [`kernel/task.c`](/Users/itoh/Github-Orthox-64/kernel/task.c)
  - scheduler logic
  - run queue policy
  - wake/sleep policy
- [`kernel/syscall.c`](/Users/itoh/Github-Orthox-64/kernel/syscall.c)
  - syscall dispatcher logic
  - mostly reusable once entry ABI is adapted
- [`kernel/fs.c`](/Users/itoh/Github-Orthox-64/kernel/fs.c)
- [`kernel/net_socket.c`](/Users/itoh/Github-Orthox-64/kernel/net_socket.c)
- [`kernel/lwip_port.c`](/Users/itoh/Github-Orthox-64/kernel/lwip_port.c)
  - timer hook and low-level net device hook need adaptation
- [`kernel/net.c`](/Users/itoh/Github-Orthox-64/kernel/net.c)
- [`kernel/spinlock.c`](/Users/itoh/Github-Orthox-64/kernel/spinlock.c)
- [`kernel/cstring.c`](/Users/itoh/Github-Orthox-64/kernel/cstring.c)
- [`kernel/cstdio.c`](/Users/itoh/Github-Orthox-64/kernel/cstdio.c)
- [`kernel/cstdlib.c`](/Users/itoh/Github-Orthox-64/kernel/cstdlib.c)

These files represent the main payoff of porting: the higher-level kernel work already done is not wasted.

### I. Userland ABI Coupling

There is one important non-kernel dependency:

- [`include/syscall.h`](/Users/itoh/Github-Orthox-64/include/syscall.h)
  - explicitly says Linux x86_64 syscall numbers

That means the current userspace ABI is tied to x86_64 Linux numbering. For RISC-V there are two reasonable options:

1. Keep Linux-compatible RISC-V syscall numbers and adapt wrappers per arch.
2. Introduce an Orthox ABI layer and translate in libc wrappers.

For minimal porting friction, option 1 is better.

## First Refactor Targets

Before writing any RISC-V code, the highest-value refactors are:

1. Split `task_context` into arch-specific saved context
2. Add generic hooks for:
   - `arch_switch_address_space()`
   - `arch_enter_user()`
   - `arch_init_cpu_local()`
   - `arch_request_resched_ipi()`
   - `arch_timer_now_ms()`
3. Move GDT/IDT/LAPIC/PIC/Limine code behind x86 platform entry points
4. Separate `virtio-net` core logic from its PCI transport
5. Remove direct `lapic_get_ticks_ms()` dependency from generic code

## Minimal First Milestone

A realistic first RISC-V milestone is not DOOM, SMP, or USB.

It is this:

- boot on QEMU `virt`
- print to UART
- timer tick works
- one user task runs
- shell starts

After that, networking and SMP become practical follow-on work.

## Summary

Orthox-64 is already in a good state for a second architecture, but only after one key realization:

- the scheduler, wait/wake, VFS, and much of the socket layer are reusable
- the current boot, MMU, trap, timer, SMP, PCI, and low-level device paths are strongly x86/PC-specific

So the correct next move is not "port everything to RISC-V immediately".

It is:

1. isolate x86/PC assumptions
2. bring up RISC-V `virt` single-core
3. reconnect the reusable kernel core
4. add networking
5. add SMP last

That route is much more tractable than a direct ARM board port.
