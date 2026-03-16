$(USER_BUILD_DIR)/crt0.o: user/crt0.S
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/syscalls.o: user/syscalls.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/syscall_wrap.o: user/syscall_wrap.S
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/user_test.o: user/user_test.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/exec_test.o: user/exec_test.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/pipe_test.o: user/pipe_test.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/sh.o: user/sh.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/gcc.o: user/gcc.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/as.o: user/as.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/ld.o: user/ld.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/loop.o: user/loop.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/cowtest.o: user/cowtest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/rotest.o: user/rotest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/testvram.o: user/testvram.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/testtime.o: user/testtime.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/testkey.o: user/testkey.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/testsound.o: user/testsound.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/mmaptest.o: user/mmaptest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/reaptest.o: user/reaptest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/robusttest.o: user/robusttest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/signaltest.o: user/signaltest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/ttytest.o: user/ttytest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/sigmasktest.o: user/sigmasktest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/sigactiontest.o: user/sigactiontest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/fchdirtest.o: user/fchdirtest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/ttylinktest.o: user/ttylinktest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/mkdirtest.o: user/mkdirtest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/wadheadtest.o: user/wadheadtest.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/tickratecheck.o: user/tickratecheck.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

USER_COMMON_OBJS = $(USER_BUILD_DIR)/syscalls.o $(USER_BUILD_DIR)/syscall_wrap.o

$(USER_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/user_test.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(EXEC_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/exec_test.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(PIPE_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/pipe_test.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(SH_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/sh.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(GCC_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/gcc.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(LOOP_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/loop.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(COW_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/cowtest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(RO_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/rotest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(VRAM_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/testvram.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(TIME_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/testtime.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(KEY_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/testkey.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(SOUND_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/testsound.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(MMAP_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/mmaptest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(REAP_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/reaptest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(ROBUST_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/robusttest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(SIGNAL_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/signaltest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(TTY_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/ttytest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(SIGMASK_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/sigmasktest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(SIGACTION_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/sigactiontest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(FCHDIR_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/fchdirtest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(TTYLINK_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/ttylinktest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(MKDIR_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/mkdirtest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(WADHEAD_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/wadheadtest.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@
$(TICKRATE_TEST_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/tickratecheck.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ $(LIBGCC) -o $@

busybox-ash: $(USER_BUILD_DIR)/syscalls.o $(USER_BUILD_DIR)/syscall_wrap.o ports/build_busybox_ash.sh ports/busybox-ash.config
	./ports/build_busybox_ash.sh
	mkdir -p rootfs/bin
	if [ -f rootfs/bin/sh ] && [ ! -f rootfs/bin/sh.orthos ]; then cp rootfs/bin/sh rootfs/bin/sh.orthos; fi
	cp $(BUSYBOX_ASH_ELF) rootfs/bin/ash
	for applet in $(BUSYBOX_ASH_APPLETS); do cp $(BUSYBOX_ASH_ELF) rootfs/bin/$$applet; done
