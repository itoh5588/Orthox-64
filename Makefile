# コンパイラ設定
CC = clang
LLD = $(shell command -v lld 2>/dev/null || command -v ld.lld 2>/dev/null || \
	if [ -x /opt/homebrew/bin/lld ]; then printf /opt/homebrew/bin/lld; \
	elif [ -x /usr/local/bin/lld ]; then printf /usr/local/bin/lld; \
	elif [ -x /opt/homebrew/opt/llvm/bin/lld ]; then printf /opt/homebrew/opt/llvm/bin/lld; \
	elif [ -x /usr/local/opt/llvm/bin/lld ]; then printf /usr/local/opt/llvm/bin/lld; \
	else printf lld; fi)
LD = $(LLD) -flavor gnu
TARGET = x86_64-elf
XGCC = $(shell command -v x86_64-elf-gcc 2>/dev/null || \
	if [ -x /opt/homebrew/bin/x86_64-elf-gcc ]; then printf /opt/homebrew/bin/x86_64-elf-gcc; \
	elif [ -x /usr/local/bin/x86_64-elf-gcc ]; then printf /usr/local/bin/x86_64-elf-gcc; \
	else printf x86_64-elf-gcc; fi)
XAR = x86_64-elf-ar
XORRISO = $(shell command -v xorriso 2>/dev/null || \
	if [ -x /opt/homebrew/bin/xorriso ]; then printf /opt/homebrew/bin/xorriso; \
	elif [ -x /usr/local/bin/xorriso ]; then printf /usr/local/bin/xorriso; \
	else printf xorriso; fi)
LLVM_CLANG = $(shell command -v clang 2>/dev/null || \
	if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then printf /opt/homebrew/opt/llvm/bin/clang; \
	elif [ -x /usr/local/opt/llvm/bin/clang ]; then printf /usr/local/opt/llvm/bin/clang; \
	else printf clang; fi)
RISCV64_CC = $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then printf /opt/homebrew/opt/llvm/bin/clang; \
	elif [ -x /usr/local/opt/llvm/bin/clang ]; then printf /usr/local/opt/llvm/bin/clang; \
	elif [ -x /opt/homebrew/bin/clang ]; then printf /opt/homebrew/bin/clang; \
	elif [ -x /usr/local/bin/clang ]; then printf /usr/local/bin/clang; \
	else printf clang; fi)
QEMU_RISCV64 = $(shell command -v qemu-system-riscv64 2>/dev/null || \
	if [ -x /opt/homebrew/bin/qemu-system-riscv64 ]; then printf /opt/homebrew/bin/qemu-system-riscv64; \
	elif [ -x /usr/local/bin/qemu-system-riscv64 ]; then printf /usr/local/bin/qemu-system-riscv64; \
	else printf qemu-system-riscv64; fi)
OPENSBI_RISCV64 = $(shell if [ -f /opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then printf /opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin; \
	elif [ -f /usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then printf /usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin; \
	else printf opensbi-riscv64-generic-fw_dynamic.bin; fi)

BUILD_DIR = build
USER_BUILD_DIR = $(BUILD_DIR)/$(LIBC_IMPL)/user
OUT_DIR = out
ISO_ROOT_DIR = $(OUT_DIR)/iso_root

# フラグ
KERNEL_CFLAGS = -target $(TARGET) -std=c11 -ffreestanding -fno-stack-protector -fno-stack-check \
	-fno-lto -fno-PIE -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
	-mcmodel=kernel -O2 -Wall -Wextra -Iinclude -Iports/lwip/src/include -MMD -MP

KERNEL_LDFLAGS = -nostdlib -static -T scripts/kernel.ld
RISCV64_CFLAGS = --target=riscv64-none-elf -march=rv64gc -mabi=lp64 -ffreestanding \
	-fno-stack-protector -fno-stack-check -fno-lto -fno-PIE -mcmodel=medany -O2 -Wall -Wextra \
	-Iinclude -MMD -MP
RISCV64_LDFLAGS = -flavor gnu -m elf64lriscv -nostdlib -static -T scripts/kernel-riscv64.ld

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
	-I$(MUSL_SYSROOT)/include -Iinclude -MMD -MP

USER_LDFLAGS = -m elf_x86_64 -nostdlib -static -Ttext 0x400000

# x86_64-elf-gcc を使って libgcc.a の正確なパスを取得
LIBGCC = $(shell $(XGCC) -print-libgcc-file-name)

# 出力ファイル名
KERNEL_ELF = $(OUT_DIR)/kernel.elf
RISCV64_KERNEL_ELF = $(OUT_DIR)/kernel-riscv64.elf
USER_ELF = user/user_test.elf
MUSL_USER_ELF = user/user_test-musl.elf
EXEC_ELF = user/exec_test.elf
PIPE_TEST_ELF = user/pipetest.elf
PIPE_STRESS_ELF = user/pipestress.elf
SMP_STRESS_ELF = user/smpstress.elf
SCHEDMIX_ELF = user/schedmix.elf
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
SHOWCPU_ELF = user/showcpu.elf
RUNQSTAT_ELF = user/runqstat.elf
TCPHELLO_ELF = user/tcphello.elf
FORKCPU_TEST_ELF = user/forkcputest.elf
FORKMODE_ELF = user/forkmode.elf
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
UDP_ECHO_TEST_ELF = user/udpecho.elf
UDP_NB_TEST_ELF = user/udpnb.elf
HTTPS_FETCH_ELF = user/httpsfetch.elf
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
BUSYBOX_ASH_APPLETS = ash sh busybox cat chmod cp echo env false head httpd ls mkdir mv printenv printf pwd rm rmdir stat tail test touch true wc
AS_SRC = ports/binutils-2.26/binutils-2.26/build/gas/as-new
LD_SRC = ports/binutils-2.26/binutils-2.26/build/ld/ld-new
AS_SRC_MUSL = ports/binutils-2.26/binutils-2.26/build-musl/gas/as-new
LD_SRC_MUSL = ports/binutils-2.26/binutils-2.26/build-musl/ld/ld-new
OSSTUBS_A = ports/libosstubs.a
ISO = $(OUT_DIR)/orthos.iso
USB_IMG = $(OUT_DIR)/usb.img
ROOTFS_TAR = $(OUT_DIR)/rootfs.tar
ROOTFS_FILES = $(shell find rootfs -type f 2>/dev/null)

# ソース
SRCS = kernel/init.c kernel/pmm.c kernel/elf.c kernel/x86_64/gdt.c kernel/x86_64/gdt_flush.S \
       kernel/x86_64/platform.c kernel/x86_64/time.c kernel/x86_64/trap.c kernel/x86_64/vm.c \
       kernel/x86_64/interrupt.S kernel/x86_64/syscall_entry.S kernel/x86_64/task_switch.S \
       kernel/vmm.c kernel/x86_64/idt.c kernel/x86_64/lapic.c kernel/sound.c kernel/syscall.c \
       kernel/task.c kernel/fs.c kernel/x86_64/pic.c kernel/keyboard.c kernel/pci.c kernel/net.c kernel/net_socket.c kernel/virtio_net.c kernel/lwip_port.c kernel/cstring.c kernel/cstdio.c kernel/cstdlib.c kernel/usb.c kernel/smp.c kernel/spinlock.c
RISCV64_C_SRCS = kernel/riscv64/boot.c kernel/riscv64/bootstrap_user.c kernel/riscv64/elf.c kernel/riscv64/entry.c kernel/riscv64/fs.c kernel/riscv64/net_socket.c kernel/riscv64/pmm.c kernel/riscv64/runtime.c kernel/riscv64/task.c kernel/riscv64/trap.c kernel/riscv64/syscall.c kernel/riscv64/vm.c
RISCV64_SHARED_C_SRCS = kernel/task.c kernel/elf.c kernel/cstring.c
RISCV64_ASM_SRCS = kernel/riscv64/start.S kernel/riscv64/trap.S kernel/riscv64/entry.S

LWIP_CORE_SRCS = \
	ports/lwip/src/core/def.c \
	ports/lwip/src/core/dns.c \
	ports/lwip/src/core/inet_chksum.c \
	ports/lwip/src/core/init.c \
	ports/lwip/src/core/ip.c \
	ports/lwip/src/core/mem.c \
	ports/lwip/src/core/memp.c \
	ports/lwip/src/core/netif.c \
	ports/lwip/src/core/pbuf.c \
	ports/lwip/src/core/raw.c \
	ports/lwip/src/core/stats.c \
	ports/lwip/src/core/sys.c \
	ports/lwip/src/core/tcp.c \
	ports/lwip/src/core/tcp_in.c \
	ports/lwip/src/core/tcp_out.c \
	ports/lwip/src/core/timeouts.c \
	ports/lwip/src/core/udp.c
LWIP_IPV4_SRCS = \
	ports/lwip/src/core/ipv4/dhcp.c \
	ports/lwip/src/core/ipv4/etharp.c \
	ports/lwip/src/core/ipv4/icmp.c \
	ports/lwip/src/core/ipv4/ip4.c \
	ports/lwip/src/core/ipv4/ip4_addr.c
LWIP_NETIF_SRCS = ports/lwip/src/netif/ethernet.c
LWIP_SRCS = $(LWIP_CORE_SRCS) $(LWIP_IPV4_SRCS) $(LWIP_NETIF_SRCS)
BEARSSL_SRCS = $(shell find ports/BearSSL/src -type f -name '*.c' 2>/dev/null)
BEARSSL_OBJS = $(patsubst ports/BearSSL/src/%.c, $(BUILD_DIR)/bearssl/%.o, $(BEARSSL_SRCS))
BEARSSL_A = $(BUILD_DIR)/bearssl/libbearssl.a

# オブジェクトファイル (build ディレクトリ以下に配置)
OBJS = $(patsubst kernel/%.c, $(BUILD_DIR)/kernel/%.o, $(filter %.c, $(SRCS))) \
       $(patsubst kernel/%.S, $(BUILD_DIR)/kernel/%.o, $(filter %.S, $(SRCS))) \
       $(patsubst ports/lwip/src/%.c, $(BUILD_DIR)/lwip/%.o, $(LWIP_SRCS))
RISCV64_OBJS = $(patsubst kernel/riscv64/%.c, $(BUILD_DIR)/riscv64/kernel/%.o, $(RISCV64_C_SRCS)) \
	       $(patsubst kernel/%.c, $(BUILD_DIR)/riscv64/kernel/shared/%.o, $(RISCV64_SHARED_C_SRCS)) \
	       $(patsubst kernel/riscv64/%.S, $(BUILD_DIR)/riscv64/kernel/%_asm.o, $(RISCV64_ASM_SRCS))

DEPS = $(OBJS:.o=.d) \
       $(USER_BUILD_DIR)/crt0.d $(USER_BUILD_DIR)/syscalls.d $(USER_BUILD_DIR)/syscalls_musl.d $(USER_BUILD_DIR)/syscall_wrap.d \
       $(USER_BUILD_DIR)/user_test.d $(USER_BUILD_DIR)/exec_test.d $(USER_BUILD_DIR)/pipe_test.d $(USER_BUILD_DIR)/pipestress.d $(USER_BUILD_DIR)/smpstress.d $(USER_BUILD_DIR)/schedmix.d \
       $(USER_BUILD_DIR)/at_test.d \
       $(USER_BUILD_DIR)/sh.d $(USER_BUILD_DIR)/gcc.d $(USER_BUILD_DIR)/as.d $(USER_BUILD_DIR)/ld.d \
       $(USER_BUILD_DIR)/loop.d $(USER_BUILD_DIR)/cowtest.d $(USER_BUILD_DIR)/rotest.d \
       $(USER_BUILD_DIR)/testvram.d $(USER_BUILD_DIR)/testtime.d $(USER_BUILD_DIR)/testkey.d \
       $(USER_BUILD_DIR)/showcpu.d $(USER_BUILD_DIR)/runqstat.d $(USER_BUILD_DIR)/tcphello.d \
       $(USER_BUILD_DIR)/testsound.d $(USER_BUILD_DIR)/mmaptest.d $(USER_BUILD_DIR)/reaptest.d \
       $(USER_BUILD_DIR)/robusttest.d $(USER_BUILD_DIR)/signaltest.d $(USER_BUILD_DIR)/ttytest.d \
       $(USER_BUILD_DIR)/sigmasktest.d $(USER_BUILD_DIR)/sigactiontest.d \
       $(USER_BUILD_DIR)/fchdirtest.d $(USER_BUILD_DIR)/ttylinktest.d \
       $(USER_BUILD_DIR)/mkdirtest.d $(USER_BUILD_DIR)/wadheadtest.d \
       $(USER_BUILD_DIR)/wadstdio_test.d $(USER_BUILD_DIR)/udpecho.d $(USER_BUILD_DIR)/udpnb.d

.PHONY: all clean run smprun smp4run netrun usb usb-img doommsulrun doommuslrun toolchain toolchain-musl user/doomgeneric.elf busybox-ash busybox-ash-musl busybox-ash-musl-install __busybox_ash_musl __busybox_ash_musl_install riscv64-run riscv64-smoke

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
	@mkdir -p $(@D)
	$(LD) $(KERNEL_LDFLAGS) $(OBJS) -o $@

$(RISCV64_KERNEL_ELF): $(RISCV64_OBJS)
	@mkdir -p $(@D)
	$(LLD) $(RISCV64_LDFLAGS) $(RISCV64_OBJS) -o $@

$(BUILD_DIR)/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/%.o: kernel/%.S
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/riscv64/kernel/%.o: kernel/riscv64/%.c
	@mkdir -p $(@D)
	$(RISCV64_CC) $(RISCV64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/riscv64/kernel/shared/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(RISCV64_CC) $(RISCV64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/riscv64/kernel/%_asm.o: kernel/riscv64/%.S
	@mkdir -p $(@D)
	$(RISCV64_CC) $(RISCV64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lwip/%.o: ports/lwip/src/%.c
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/bearssl/%.o: ports/BearSSL/src/%.c
	@mkdir -p $(@D)
	$(CC) -target $(TARGET) -std=c99 -ffreestanding -fno-PIE -O2 -Iports/BearSSL/inc -Iports/BearSSL/src -I$(MUSL_SYSROOT)/include -MMD -MP -c $< -o $@

$(BEARSSL_A): $(BEARSSL_OBJS)
	@mkdir -p $(@D)
	ar rcs $@ $^

limine/limine:
	CC=cc CFLAGS="-O2 -pipe" LDFLAGS="" $(MAKE) -C limine limine

TEST_ELFS = $(MMAP_TEST_ELF) $(REAP_TEST_ELF) $(ROBUST_TEST_ELF) $(VRAM_TEST_ELF) $(TIME_TEST_ELF) $(KEY_TEST_ELF) $(SOUND_TEST_ELF) $(SIGNAL_TEST_ELF) $(TTY_TEST_ELF) $(SIGMASK_TEST_ELF) $(SIGACTION_TEST_ELF) $(FCHDIR_TEST_ELF) $(TTYLINK_TEST_ELF) $(MKDIR_TEST_ELF) $(WADHEAD_TEST_ELF)

FORCE:

$(ROOTFS_TAR): FORCE busybox-ash-musl-install $(ROOTFS_FILES) $(BUILD_DIR)/musl/user/crt0.o $(BUILD_DIR)/musl/user/syscalls_musl.o $(UDP_ECHO_TEST_ELF) $(UDP_NB_TEST_ELF) $(HTTPS_FETCH_ELF) $(TIME_TEST_ELF) $(TICKRATE_TEST_ELF) $(SHOWCPU_ELF) $(RUNQSTAT_ELF) $(FORKCPU_TEST_ELF) $(FORKMODE_ELF) $(PIPE_STRESS_ELF) $(SMP_STRESS_ELF) $(SCHEDMIX_ELF) $(REAP_TEST_ELF) $(DOOM_MUSL_ELF)
	@mkdir -p $(@D)
	mkdir -p rootfs/bin
	# Install musl development files
	cp $(BUILD_DIR)/musl/user/crt0.o rootfs/crt0.o
	cp $(BUILD_DIR)/musl/user/syscalls_musl.o rootfs/syscalls.o
	cp $(MUSL_SYSROOT)/lib/libc.a rootfs/libc.a
	cp $(UDP_ECHO_TEST_ELF) rootfs/bin/udpecho.elf
	cp $(UDP_NB_TEST_ELF) rootfs/bin/udpnb.elf
	cp $(HTTPS_FETCH_ELF) rootfs/bin/httpsfetch.elf
	cp $(TIME_TEST_ELF) rootfs/bin/testtime.elf
	cp $(TICKRATE_TEST_ELF) rootfs/bin/tickratecheck.elf
	cp $(SHOWCPU_ELF) rootfs/bin/showcpu.elf
	cp $(RUNQSTAT_ELF) rootfs/bin/runqstat.elf
	cp $(TCPHELLO_ELF) rootfs/bin/tcphello.elf
	cp $(DOOM_MUSL_ELF) rootfs/bin/doom-musl.elf
	cp $(FORKCPU_TEST_ELF) rootfs/bin/forkcputest.elf
	cp $(FORKMODE_ELF) rootfs/bin/forkmode.elf
	cp $(PIPE_STRESS_ELF) rootfs/bin/pipestress.elf
	cp $(SMP_STRESS_ELF) rootfs/bin/smpstress.elf
	cp $(SCHEDMIX_ELF) rootfs/bin/schedmix.elf
	cp $(REAP_TEST_ELF) rootfs/bin/reaptest.elf
	# Remove old bin-musl if it exists to avoid confusion
	rm -rf rootfs/bin-musl
	tar --format=ustar -cf $(ROOTFS_TAR) -C rootfs .

$(ISO): $(KERNEL_ELF) $(SH_ELF) $(DOOM_MUSL_ELF) iso/limine.conf limine/limine $(ROOTFS_TAR)
	rm -rf $(ISO_ROOT_DIR)
	mkdir -p $(ISO_ROOT_DIR)/boot/limine
	cp $(KERNEL_ELF) $(ISO_ROOT_DIR)/boot/kernel.elf
	cp $(SH_ELF) $(ISO_ROOT_DIR)/boot/sh.elf
	cp $(DOOM_MUSL_ELF) $(ISO_ROOT_DIR)/boot/doom-musl.elf
	cp $(ROOTFS_TAR) $(ISO_ROOT_DIR)/boot/rootfs.tar
	cp iso/limine.conf $(ISO_ROOT_DIR)/boot/limine/limine.conf

	mkdir -p $(ISO_ROOT_DIR)/EFI/BOOT
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin $(ISO_ROOT_DIR)/boot/limine/
	cp limine/BOOTX64.EFI $(ISO_ROOT_DIR)/EFI/BOOT/
	cp limine/BOOTIA32.EFI $(ISO_ROOT_DIR)/EFI/BOOT/
	$(XORRISO) -as mkisofs -v -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_ROOT_DIR) -o $(ISO)
	$(CURDIR)/limine/limine bios-install $(ISO)
	rm -rf $(ISO_ROOT_DIR)

run: $(ISO)
	bash ./run_qemu_stdio.sh

riscv64-run: $(RISCV64_KERNEL_ELF)
	bash ./run_qemu_riscv64.sh

riscv64-smoke: $(RISCV64_KERNEL_ELF)
	bash ./tests/riscv64_smoke.sh

smprun: $(ISO)
	bash ./run_qemu_stdio.sh \
		-smp 2

smp4run: $(ISO)
	bash ./run_qemu_stdio.sh \
		-smp 4

netrun: $(ISO)
	bash ./run_qemu_stdio.sh \
		-netdev user,id=net0,hostfwd=tcp::8080-:8080,hostfwd=udp::12345-:12345,hostfwd=udp::12346-:12346 \
		-device virtio-net-pci,netdev=net0

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
	bash ./run_qemu_stdio.sh \
		-device qemu-xhci,id=xhci \
		-drive if=none,id=usbdisk,file=$(USB_IMG),format=raw \
		-device usb-storage,bus=xhci.0,drive=usbdisk

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR) $(USER_ELF) $(EXEC_ELF) $(PIPE_TEST_ELF) $(SH_ELF) $(GCC_ELF) $(LOOP_ELF)
	rm -f $(MUSL_USER_ELF) $(MUSL_SH_ELF)
	rm -f user/*.elf
	$(MAKE) -C limine clean

-include $(DEPS)
