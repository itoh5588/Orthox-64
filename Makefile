# コンパイラ設定
CC = clang
LD = lld -flavor gnu
TARGET = x86_64-elf
XGCC = x86_64-elf-gcc
XAR = x86_64-elf-ar

BUILD_DIR = build
USER_BUILD_DIR = $(BUILD_DIR)/$(LIBC_IMPL)/user

# フラグ
KERNEL_CFLAGS = -target $(TARGET) -std=c11 -ffreestanding -fno-stack-protector -fno-stack-check \
	-fno-lto -fno-PIE -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
	-mcmodel=kernel -O2 -Wall -Wextra -Iinclude -MMD -MP

KERNEL_LDFLAGS = -nostdlib -static -T scripts/kernel.ld

# libc / sysroot 設定
LIBC_IMPL ?= musl
NEWLIB_SYSROOT = user
MUSL_SYSROOT ?= ports/musl-install

ifeq ($(LIBC_IMPL),musl)
USER_SYSROOT = $(MUSL_SYSROOT)
else
USER_SYSROOT = $(NEWLIB_SYSROOT)
endif

USER_INCLUDEDIR = $(USER_SYSROOT)/include
ifeq ($(LIBC_IMPL),musl)
USER_LIBDIR = $(USER_SYSROOT)/lib
else
USER_LIBDIR = $(USER_SYSROOT)/libs
endif
LIBC = $(USER_LIBDIR)/libc.a

# ユーザープログラム用フラグ
USER_CFLAGS = -target $(TARGET) -std=c11 -ffreestanding -fno-PIE -O2 \
	-Iinclude -I$(USER_INCLUDEDIR) -MMD -MP
MUSL_USER_CFLAGS = -target $(TARGET) -std=c11 -ffreestanding -fno-PIE -O2 \
	-Iinclude -I$(MUSL_SYSROOT)/include -MMD -MP

USER_LDFLAGS = -m elf_x86_64 -nostdlib -static -Ttext 0x400000

# x86_64-elf-gcc を使って libgcc.a の正確なパスを取得
LIBGCC = $(shell x86_64-elf-gcc -print-libgcc-file-name)

# 出力ファイル名
KERNEL_ELF = kernel.elf
USER_ELF = user/user_test.elf
MUSL_USER_ELF = user/user_test-musl.elf
EXEC_ELF = user/exec_test.elf
PIPE_TEST_ELF = user/pipetest.elf
SH_ELF = user/sh.elf
MUSL_SH_ELF = user/sh-musl.elf
AT_TEST_MUSL_ELF = user/attest-musl.elf
WADSTDIO_TEST_MUSL_ELF = user/wadstdio-musl.elf
GCC_ELF = user/gcc.elf
GCC_MUSL_ELF = user/gcc-musl.elf
LOOP_ELF = user/loop.elf
COW_TEST_ELF = user/cowtest.elf
RO_TEST_ELF = user/rotest.elf
VRAM_TEST_ELF = user/testvram.elf
TIME_TEST_ELF = user/testtime.elf
KEY_TEST_ELF = user/testkey.elf
SOUND_TEST_ELF = user/testsound.elf
MMAP_TEST_ELF = user/mmaptest.elf
REAP_TEST_ELF = user/reaptest.elf
ROBUST_TEST_ELF = user/robusttest.elf
SIGNAL_TEST_ELF = user/signaltest.elf
TTY_TEST_ELF = user/ttytest.elf
SIGMASK_TEST_ELF = user/sigmasktest.elf
SIGACTION_TEST_ELF = user/sigactiontest.elf
FCHDIR_TEST_ELF = user/fchdirtest.elf
TTYLINK_TEST_ELF = user/ttylinktest.elf
MKDIR_TEST_ELF = user/mkdirtest.elf
WADHEAD_TEST_ELF = user/wadheadtest.elf
TICKRATE_TEST_ELF = user/tickratecheck.elf
CC1_ELF = ports/gcc-4.7.4/build/gcc/cc1
CC1_SRC_MUSL = ports/gcc-4.7.4/build-musl/gcc/cc1
CC1_MUSL_ELF = user/cc1-musl.elf
AS_ELF = user/as.elf
AS_MUSL_ELF = user/as-musl.elf
LD_ELF = user/ld.elf
LD_MUSL_ELF = user/ld-musl.elf
DOOM_ELF = user/doomgeneric.elf
DOOM_MUSL_ELF = user/doomgeneric-musl.elf
BUSYBOX_ASH_ELF = user/busybox-ash.elf
BUSYBOX_ASH_MUSL_ELF = user/busybox-ash-musl.elf
BUSYBOX_ASH_APPLETS = ash sh busybox cat chmod cp echo env false head ls mkdir mv printenv printf pwd rm rmdir stat tail test touch true wc
AS_SRC = ports/binutils-2.26/binutils-2.26/build/gas/as-new
LD_SRC = ports/binutils-2.26/binutils-2.26/build/ld/ld-new
AS_SRC_MUSL = ports/binutils-2.26/binutils-2.26/build-musl/gas/as-new
LD_SRC_MUSL = ports/binutils-2.26/binutils-2.26/build-musl/ld/ld-new
OSSTUBS_A = ports/libosstubs.a
ISO = orthos.iso
USB_IMG = usb.img
ROOTFS_TAR = rootfs.tar
ROOTFS_FILES = $(shell find rootfs -type f 2>/dev/null)

# ソース
SRCS = kernel/main.c kernel/pmm.c kernel/elf.c kernel/gdt.c kernel/gdt_flush.S \
       kernel/vmm.c kernel/idt.c kernel/interrupt.S kernel/lapic.c kernel/sound.c kernel/syscall.c kernel/syscall_entry.S \
       kernel/task.c kernel/task_switch.S kernel/fs.c kernel/pic.c kernel/keyboard.c kernel/pci.c kernel/usb.c

# オブジェクトファイル (build ディレクトリ以下に配置)
OBJS = $(patsubst kernel/%.c, $(BUILD_DIR)/kernel/%.o, $(filter %.c, $(SRCS))) \
       $(patsubst kernel/%.S, $(BUILD_DIR)/kernel/%.o, $(filter %.S, $(SRCS)))

DEPS = $(OBJS:.o=.d) \
       $(USER_BUILD_DIR)/crt0.d $(USER_BUILD_DIR)/syscalls.d $(USER_BUILD_DIR)/syscalls_musl.d $(USER_BUILD_DIR)/syscall_wrap.d \
       $(USER_BUILD_DIR)/user_test.d $(USER_BUILD_DIR)/exec_test.d $(USER_BUILD_DIR)/pipe_test.d \
       $(USER_BUILD_DIR)/at_test.d \
       $(USER_BUILD_DIR)/sh.d $(USER_BUILD_DIR)/gcc.d $(USER_BUILD_DIR)/as.d $(USER_BUILD_DIR)/ld.d \
       $(USER_BUILD_DIR)/loop.d $(USER_BUILD_DIR)/cowtest.d $(USER_BUILD_DIR)/rotest.d \
       $(USER_BUILD_DIR)/testvram.d $(USER_BUILD_DIR)/testtime.d $(USER_BUILD_DIR)/testkey.d \
       $(USER_BUILD_DIR)/testsound.d $(USER_BUILD_DIR)/mmaptest.d $(USER_BUILD_DIR)/reaptest.d \
       $(USER_BUILD_DIR)/robusttest.d $(USER_BUILD_DIR)/signaltest.d $(USER_BUILD_DIR)/ttytest.d \
       $(USER_BUILD_DIR)/sigmasktest.d $(USER_BUILD_DIR)/sigactiontest.d \
       $(USER_BUILD_DIR)/fchdirtest.d $(USER_BUILD_DIR)/ttylinktest.d \
       $(USER_BUILD_DIR)/mkdirtest.d $(USER_BUILD_DIR)/wadheadtest.d \
       $(USER_BUILD_DIR)/wadstdio_test.d

.PHONY: all clean run usb usb-img doommsulrun doommuslrun toolchain toolchain-musl user/doomgeneric.elf busybox-ash busybox-ash-musl busybox-ash-musl-install __busybox_ash_musl __busybox_ash_musl_install

all: $(ISO)

include mk/user-$(LIBC_IMPL).mk

user/crt0.o: user/crt0.S
	$(XGCC) -std=c11 -ffreestanding -fno-PIE -O2 -Iinclude -Iuser/include -c $< -o $@

user/syscalls.o: user/syscalls.c
	$(XGCC) -std=c11 -ffreestanding -fno-PIE -O2 -Iinclude -Iuser/include -c $< -o $@

ports/user_stubs.o: ports/user_stubs.c
	$(XGCC) -std=c11 -ffreestanding -fno-PIE -O2 -Iinclude -Iuser/include -c $< -o $@

$(OSSTUBS_A): ports/user_stubs.o
	rm -f $@
	$(XAR) rcs $@ $^

ifeq ($(LIBC_IMPL),musl)
toolchain: toolchain-musl
else
$(AS_SRC): user/crt0.o user/syscalls.o
	rm -f $@
	$(MAKE) -C ports/binutils-2.26/binutils-2.26/build/gas as-new

$(LD_SRC): user/crt0.o user/syscalls.o $(AS_SRC)
	rm -f $@
	$(MAKE) -C ports/binutils-2.26/binutils-2.26/build/ld ld-new

$(CC1_ELF): user/crt0.o user/syscalls.o $(OSSTUBS_A)
	rm -f $@
	$(MAKE) -C ports/gcc-4.7.4/build/gcc cc1

toolchain: $(CC1_ELF) $(AS_SRC) $(LD_SRC)

$(AS_ELF): $(AS_SRC)
	cp $(AS_SRC) $@

$(LD_ELF): $(LD_SRC)
	cp $(LD_SRC) $@
endif

user/doomgeneric.elf:
	$(MAKE) -C user/doomgeneric/doomgeneric
	cp user/doomgeneric/doomgeneric/doomgeneric.elf user/doomgeneric.elf

$(DOOM_MUSL_ELF): FORCE
	$(MAKE) -C user/doomgeneric/doomgeneric LIBC_IMPL=musl OUTPUT=doomgeneric-musl.elf
	cp user/doomgeneric/doomgeneric/doomgeneric-musl.elf $(DOOM_MUSL_ELF)

busybox-ash-musl:
	$(MAKE) -C $(CURDIR) LIBC_IMPL=musl __busybox_ash_musl

busybox-ash-musl-install:
	$(MAKE) -C $(CURDIR) LIBC_IMPL=musl __busybox_ash_musl_install

ifneq ($(LIBC_IMPL),musl)
$(AT_TEST_MUSL_ELF): FORCE
	$(MAKE) -C $(CURDIR) LIBC_IMPL=musl $(AT_TEST_MUSL_ELF)

$(WADSTDIO_TEST_MUSL_ELF): FORCE
	$(MAKE) -C $(CURDIR) LIBC_IMPL=musl $(WADSTDIO_TEST_MUSL_ELF)
endif

$(KERNEL_ELF): $(OBJS)
	$(LD) $(KERNEL_LDFLAGS) $(OBJS) -o $@

$(BUILD_DIR)/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/%.o: kernel/%.S
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

limine/limine:
	CC=cc CFLAGS="-O2 -pipe" LDFLAGS="" $(MAKE) -C limine limine

TEST_ELFS = $(MMAP_TEST_ELF) $(REAP_TEST_ELF) $(ROBUST_TEST_ELF) $(VRAM_TEST_ELF) $(TIME_TEST_ELF) $(KEY_TEST_ELF) $(SOUND_TEST_ELF) $(SIGNAL_TEST_ELF) $(TTY_TEST_ELF) $(SIGMASK_TEST_ELF) $(SIGACTION_TEST_ELF) $(FCHDIR_TEST_ELF) $(TTYLINK_TEST_ELF) $(MKDIR_TEST_ELF) $(WADHEAD_TEST_ELF)

FORCE:

$(ROOTFS_TAR): FORCE busybox-ash-musl-install $(ROOTFS_FILES) $(BUILD_DIR)/musl/user/crt0.o $(BUILD_DIR)/musl/user/syscalls_musl.o
	mkdir -p rootfs/bin
	# Install musl development files
	cp $(BUILD_DIR)/musl/user/crt0.o rootfs/crt0.o
	cp $(BUILD_DIR)/musl/user/syscalls_musl.o rootfs/syscalls.o
	cp $(MUSL_SYSROOT)/lib/libc.a rootfs/libc.a
	# Remove old bin-musl if it exists to avoid confusion
	rm -rf rootfs/bin-musl
	tar --format=ustar -cf $(ROOTFS_TAR) -C rootfs .

$(ISO): $(KERNEL_ELF) $(SH_ELF) iso/limine.conf limine/limine $(ROOTFS_TAR)
	rm -rf iso_root
	mkdir -p iso_root/boot/limine
	cp $(KERNEL_ELF) iso_root/boot/kernel.elf
	cp $(SH_ELF) iso_root/boot/sh.elf
	cp $(ROOTFS_TAR) iso_root/boot/rootfs.tar
	cp iso/limine.conf iso_root/boot/limine/limine.conf

	mkdir -p iso_root/EFI/BOOT
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -v -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO)
	$(CURDIR)/limine/limine bios-install $(ISO)
	rm -rf iso_root

run: $(ISO)
	bash ./run_qemu_stdio.sh

doommsulrun: $(ISO)
	bash ./run_doom_musl.sh

doommuslrun: doommsulrun

usb-img:
	@if [ ! -f $(USB_IMG) ]; then \
		echo "Creating $(USB_IMG) (256M)"; \
		truncate -s 256M $(USB_IMG); \
	fi
	@python3 scripts/make_usb_img.py $(USB_IMG)
	@printf 'OrthOS USB TEST BLOCK 0002\nREAD(10) verification pattern\n' | \
		dd of=$(USB_IMG) bs=512 seek=2 conv=notrunc status=none
	@printf 'OrthOS USB TEST BLOCK 0003\nThird sector marker\n' | \
		dd of=$(USB_IMG) bs=512 seek=3 conv=notrunc status=none

usb: $(ISO) usb-img
	qemu-system-x86_64 \
		-machine pc,pcspk-audiodev=audio0 \
		-audiodev coreaudio,id=audio0 \
		-device sb16,audiodev=audio0 \
		-device qemu-xhci,id=xhci \
		-drive if=none,id=usbdisk,file=$(USB_IMG),format=raw \
		-device usb-storage,bus=xhci.0,drive=usbdisk \
		-cpu max -m 2G -cdrom $(ISO) -boot d -serial stdio

clean:
	rm -rf $(BUILD_DIR) $(KERNEL_ELF) $(USER_ELF) $(EXEC_ELF) $(PIPE_TEST_ELF) $(SH_ELF) $(GCC_ELF) $(LOOP_ELF) $(ISO)
	rm -f $(MUSL_USER_ELF) $(MUSL_SH_ELF)
	rm -f user/*.elf
	rm -rf iso_root
	$(MAKE) -C limine clean

-include $(DEPS)
