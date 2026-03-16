# Orthox-64 Installation Guide

## Requirements
To build this project, you need the following tools:
- **Host OS:** Linux or macOS (WSL2 recommended)
- **Compiler:** x86_64-elf-gcc, x86_64-elf-binutils
- **Build Tools:** make, python3
- **Image Creation:** xorriso, mtools
- **Emulator:** QEMU (x86_64)

## Build Instructions

1. **Clone the repository**
   ```bash
   git clone https://github.com/yourusername/Orthox-64.git
   cd Orthox-64
   ```

2. **Build the kernel and userland**
   ```bash
   make
   ```
   This will build the kernel (`kernel/kernel.elf`) and userland binaries, then create `rootfs.tar`.

3. **Create a bootable image (ISO)**
   ```bash
   make iso
   ```
   This creates a bootable ISO image including the Limine bootloader.

## Running the OS

To run using QEMU, you can use the following script:
```bash
./run_qemu.sh
```
(Use `./run_doom_qemu.sh` to run DOOM)

## Toolchain Setup
If you need to build your own toolchain, use the scripts in the `ports/` directory:
```bash
cd ports
./build_gcc.sh
./build_binutils.sh
```
