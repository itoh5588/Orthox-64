MUSL_USER_CFLAGS = -target $(TARGET) -std=c11 -ffreestanding -fno-PIE -O2 -Wno-shift-op-parentheses \
	-I$(MUSL_SYSROOT)/include -Iinclude -MMD -MP

$(USER_BUILD_DIR)/crt0.o: user/crt0_musl.S
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/syscalls_musl.o: user/syscalls_musl.c
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/syscall_wrap.o: user/syscall_wrap.S
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/user_test.o: user/user_test.c
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/at_test.o: user/at_test.c
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/wadstdio_test.o: user/wadstdio_test.c
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/sh.o: user/sh.c
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/gcc.o: user/gcc.c
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

USER_COMMON_OBJS = $(USER_BUILD_DIR)/syscalls_musl.o $(USER_BUILD_DIR)/syscall_wrap.o

$(USER_BUILD_DIR)/%.o: user/%.c
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

user/pipetest.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/pipe_test.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@

user/%.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/%.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@

$(MUSL_USER_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/user_test.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@

$(USER_ELF): $(MUSL_USER_ELF)
	cp $(MUSL_USER_ELF) $(USER_ELF)

$(AT_TEST_MUSL_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/at_test.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@

$(WADSTDIO_TEST_MUSL_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/wadstdio_test.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@

$(MUSL_SH_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/sh.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@

$(SH_ELF): $(MUSL_SH_ELF)
	cp $(MUSL_SH_ELF) $(SH_ELF)

$(GCC_MUSL_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/gcc.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@

$(GCC_ELF): $(GCC_MUSL_ELF)
	cp $(GCC_MUSL_ELF) $(GCC_ELF)

$(CC1_MUSL_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/syscalls_musl.o $(USER_BUILD_DIR)/syscall_wrap.o $(OSSTUBS_A) $(LIBC) ports/build_gcc_musl.sh
	ORTHOS_SYSROOT=$(abspath $(MUSL_SYSROOT)) ORTHOS_INCLUDEDIR=$(abspath $(MUSL_SYSROOT))/include ORTHOS_LIBDIR=$(abspath $(MUSL_SYSROOT))/lib ORTHOS_CRT0=$(abspath $(USER_BUILD_DIR)/crt0.o) ORTHOS_SYSCALLS_O=$(abspath $(USER_BUILD_DIR)/syscalls_musl.o) ORTHOS_SYSCALL_WRAP_O=$(abspath $(USER_BUILD_DIR)/syscall_wrap.o) ./ports/build_gcc_musl.sh
	cp $(CC1_SRC_MUSL) $@

$(CC1_ELF): $(CC1_MUSL_ELF)
	cp $(CC1_MUSL_ELF) $(CC1_ELF)

$(AS_MUSL_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/syscalls_musl.o $(USER_BUILD_DIR)/syscall_wrap.o $(LIBC)
	ORTHOS_SYSROOT=$(abspath $(MUSL_SYSROOT)) ORTHOS_INCLUDEDIR=$(abspath $(MUSL_SYSROOT))/include ORTHOS_LIBDIR=$(abspath $(MUSL_SYSROOT))/lib ORTHOS_CRT0=$(abspath $(USER_BUILD_DIR)/crt0.o) ORTHOS_SYSCALLS_O=$(abspath $(USER_BUILD_DIR)/syscalls_musl.o) ORTHOS_SYSCALL_WRAP_O=$(abspath $(USER_BUILD_DIR)/syscall_wrap.o) ./ports/build_binutils_musl.sh
	cp $(AS_SRC_MUSL) $@

$(AS_ELF): $(AS_MUSL_ELF)
	cp $(AS_MUSL_ELF) $(AS_ELF)

$(LD_MUSL_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/syscalls_musl.o $(USER_BUILD_DIR)/syscall_wrap.o $(AS_MUSL_ELF) $(LIBC)
	ORTHOS_SYSROOT=$(abspath $(MUSL_SYSROOT)) ORTHOS_INCLUDEDIR=$(abspath $(MUSL_SYSROOT))/include ORTHOS_LIBDIR=$(abspath $(MUSL_SYSROOT))/lib ORTHOS_CRT0=$(abspath $(USER_BUILD_DIR)/crt0.o) ORTHOS_SYSCALLS_O=$(abspath $(USER_BUILD_DIR)/syscalls_musl.o) ORTHOS_SYSCALL_WRAP_O=$(abspath $(USER_BUILD_DIR)/syscall_wrap.o) ./ports/build_binutils_musl.sh
	cp $(LD_SRC_MUSL) $@

$(LD_ELF): $(LD_MUSL_ELF)
	cp $(LD_MUSL_ELF) $(LD_ELF)

toolchain-musl: $(CC1_MUSL_ELF) $(AS_MUSL_ELF) $(LD_MUSL_ELF) $(GCC_MUSL_ELF)

$(BUSYBOX_ASH_MUSL_ELF): $(USER_BUILD_DIR)/syscalls_musl.o $(USER_BUILD_DIR)/syscall_wrap.o ports/build_busybox_ash.sh ports/busybox-ash.config
	ORTHOS_SYSROOT=$(abspath $(MUSL_SYSROOT)) ORTHOS_INCLUDEDIR=$(abspath $(MUSL_SYSROOT))/include ORTHOS_LIBDIR=$(abspath $(MUSL_SYSROOT))/lib ORTHOS_CRT0=$(abspath $(USER_BUILD_DIR)/crt0.o) ORTHOS_SYSCALLS_O=$(abspath $(USER_BUILD_DIR)/syscalls_musl.o) ORTHOS_SYSCALL_WRAP_O=$(abspath $(USER_BUILD_DIR)/syscall_wrap.o) ./ports/build_busybox_ash.sh $(abspath ports/busybox) $(abspath $(BUSYBOX_ASH_MUSL_ELF))

__busybox_ash_musl: $(BUSYBOX_ASH_MUSL_ELF)

__busybox_ash_musl_install: $(BUSYBOX_ASH_MUSL_ELF) $(AT_TEST_MUSL_ELF)
	mkdir -p rootfs/bin
	# Install musl busybox and applets directly to rootfs/bin
	cat $(BUSYBOX_ASH_MUSL_ELF) > rootfs/bin/busybox && chmod +x rootfs/bin/busybox
	for applet in $(BUSYBOX_ASH_APPLETS); do cat $(BUSYBOX_ASH_MUSL_ELF) > rootfs/bin/$$applet && chmod +x rootfs/bin/$$applet; done
	cat $(BUSYBOX_ASH_MUSL_ELF) > rootfs/bin/ash.musl && chmod +x rootfs/bin/ash.musl
	cat $(BUSYBOX_ASH_MUSL_ELF) > rootfs/bin/sh.musl && chmod +x rootfs/bin/sh.musl
	cat $(AT_TEST_MUSL_ELF) > rootfs/bin/attest-musl.elf && chmod +x rootfs/bin/attest-musl.elf
