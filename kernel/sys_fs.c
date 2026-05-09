#include <stddef.h>
#include <stdint.h>
#include "fs.h"
#include "sys_internal.h"
#include "task.h"

#define ORTH_TCGETS      0x5401
#define ORTH_TCSETS      0x5402
#define ORTH_TIOCGPGRP   0x540F
#define ORTH_TIOCSPGRP   0x5410
#define ORTH_TIOCGWINSZ  0x5413

#ifndef EINVAL
#define EINVAL 22
#endif

#ifndef ORTHOX_MEM_PROGRESS
#define ORTHOX_MEM_PROGRESS 0
#endif

static struct orth_termios g_console_termios = {
    .c_iflag = 0x00000002u,
    .c_oflag = 0x00000001u,
    .c_cflag = 0,
    .c_lflag = 0x00000001u | 0x00000002u | 0x00000008u,
    .c_cc = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26 },
    .c_ispeed = 115200,
    .c_ospeed = 115200,
};

int sys_open(const char* path, int flags, int mode) {
    return fs_open(path, flags, mode);
}

int sys_openat(int dirfd, const char* path, int flags, int mode) {
    return fs_openat(dirfd, path, flags, mode);
}

int64_t sys_read(int fd, void* buf, size_t count) {
    return fs_read(fd, buf, count);
}

int64_t sys_write(int fd, const void* buf, size_t count) {
    return fs_write(fd, buf, count);
}

int sys_close(int fd) {
    return fs_close(fd);
}

int sys_fcntl(int fd, int cmd, uint64_t arg) {
    return fs_fcntl(fd, cmd, arg);
}

int sys_pipe(int pipefd[2]) {
    return fs_pipe(pipefd);
}

int sys_pipe2(int pipefd[2], int flags) {
    return fs_pipe2(pipefd, flags);
}

int sys_dup2(int oldfd, int newfd) {
    return fs_dup2(oldfd, newfd);
}

int sys_fstat(int fd, struct kstat* st) {
    return fs_fstat(fd, st);
}

int sys_stat(const char* path, struct kstat* st) {
    return fs_stat(path, st);
}

int sys_fstatat(int dirfd, const char* path, struct kstat* st, int flags) {
    return fs_fstatat(dirfd, path, st, flags);
}

int sys_access(const char* path, int mode) {
    return fs_access(path, mode);
}

int sys_faccessat(int dirfd, const char* path, int mode, int flags) {
    return fs_faccessat(dirfd, path, mode, flags);
}

int64_t sys_readlink(const char* path, char* buf, size_t bufsiz) {
    return fs_readlink(path, buf, bufsiz);
}

int64_t sys_readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz) {
    return fs_readlinkat(dirfd, path, buf, bufsiz);
}

int64_t sys_lseek(int fd, int64_t offset, int whence) {
    return fs_lseek(fd, offset, whence);
}

int sys_getdents(int fd, struct orth_dirent* dirp, size_t count) {
    return fs_getdents(fd, dirp, count);
}

int sys_getdents64(int fd, void* dirp, size_t count) {
    return fs_getdents64(fd, dirp, count);
}

int sys_chdir(const char* path) {
    return fs_chdir(path);
}

int sys_fchdir(int fd) {
    return fs_fchdir(fd);
}

int sys_getcwd(char* buf, size_t size) {
    return fs_getcwd(buf, size);
}

int sys_truncate(const char* path, uint64_t length) {
    return fs_truncate(path, length);
}

int sys_ftruncate(int fd, uint64_t length) {
    return fs_ftruncate(fd, length);
}

int sys_utimensat(int dirfd, const char* path, const void* times, int flags) {
    return fs_utimensat(dirfd, path, times, flags);
}

int sys_sync(void) {
    return fs_sync();
}

int sys_unlink(const char* path) {
    return fs_unlink(path);
}

int sys_unlinkat(int dirfd, const char* path, int flags) {
    return fs_unlinkat(dirfd, path, flags);
}

int sys_rename(const char* oldpath, const char* newpath) {
    return fs_rename(oldpath, newpath);
}

int sys_chmod(const char* path, uint32_t mode) {
    return fs_chmod(path, mode);
}

int sys_mkdir(const char* path, int mode) {
    return fs_mkdir(path, mode);
}

int sys_mkdirat(int dirfd, const char* path, int mode) {
    return fs_mkdirat(dirfd, path, mode);
}

int sys_rmdir(const char* path) {
    return fs_rmdir(path);
}

int64_t sys_pread64(int fd, void* buf, size_t count, int64_t offset) {
    int64_t old_offset;
    int64_t ret;
    if (offset < 0) return -EINVAL;
    old_offset = fs_lseek(fd, 0, 1);
    if (old_offset < 0) return old_offset;
    if (fs_lseek(fd, offset, 0) < 0) {
        fs_lseek(fd, old_offset, 0);
        return -1;
    }
    ret = fs_read(fd, buf, count);
    fs_lseek(fd, old_offset, 0);
    return ret;
}

int64_t sys_pwrite64(int fd, const void* buf, size_t count, int64_t offset) {
    int64_t old_offset;
    int64_t ret;
    if (offset < 0) return -EINVAL;
    old_offset = fs_lseek(fd, 0, 1);
    if (old_offset < 0) return old_offset;
    if (fs_lseek(fd, offset, 0) < 0) {
        fs_lseek(fd, old_offset, 0);
        return -1;
    }
    ret = fs_write(fd, buf, count);
    fs_lseek(fd, old_offset, 0);
    return ret;
}

int sys_tcgetattr(int fd, struct orth_termios* tio) {
    (void)fd;
    if (!tio) return -1;
    *tio = g_console_termios;
    return 0;
}

int sys_tcsetattr(int fd, int optional_actions, const struct orth_termios* tio) {
    (void)fd;
    (void)optional_actions;
    if (!tio) return -1;
    g_console_termios = *tio;
    return 0;
}

int sys_ioctl(int fd, unsigned long request, uint64_t arg) {
    switch (request) {
        case ORTH_TIOCGWINSZ:
            if (!arg) return -1;
            ((struct orth_winsize*)arg)->ws_row = 25;
            ((struct orth_winsize*)arg)->ws_col = 80;
            ((struct orth_winsize*)arg)->ws_xpixel = 0;
            ((struct orth_winsize*)arg)->ws_ypixel = 0;
            return 0;
        case ORTH_TIOCGPGRP:
            if (!arg) return -1;
            *(int*)arg = sys_tcgetpgrp(fd);
            return 0;
        case ORTH_TIOCSPGRP:
            if (!arg) return -1;
            return sys_tcsetpgrp(fd, *(const int*)arg);
        case ORTH_TCGETS:
            return sys_tcgetattr(fd, (struct orth_termios*)arg);
        case ORTH_TCSETS:
            return sys_tcsetattr(fd, 0, (const struct orth_termios*)arg);
        default:
            return -1;
    }
}

int sys_lstat(const char* path, struct kstat* st) {
    return fs_stat(path, st);
}

int64_t sys_writev(int fd, const struct orth_iovec* iov, int iovcnt) {
    int64_t total = 0;
    if (!iov || iovcnt < 0) return -1;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = fs_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : rc;
        total += rc;
        if ((size_t)rc != iov[i].iov_len) break;
    }
#if ORTHOX_MEM_PROGRESS
    {
        struct task* current = get_current_task();
        if (current && current->trace_progress && total > 0) {
            current->trace_write_bytes += (uint64_t)total;
            if ((uint64_t)total > current->trace_write_max) {
                current->trace_write_max = (uint64_t)total;
            }
        }
    }
#endif
    return total;
}

int64_t sys_readv(int fd, const struct orth_iovec* iov, int iovcnt) {
    int64_t total = 0;
    if (!iov || iovcnt < 0) return -1;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = fs_read(fd, (void*)iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : rc;
        total += rc;
        if ((size_t)rc != iov[i].iov_len) break;
    }
    return total;
}

int sys_mount_module_root(void) {
    return fs_mount_module_root();
}

int sys_get_mount_status(char* buf, size_t size) {
    return fs_get_mount_status(buf, size);
}

int sys_pipe_user(int* user_pipefd) {
    int pipefd[2];
    int ret = fs_pipe(pipefd);
    if (ret == 0) {
        user_pipefd[0] = pipefd[0];
        user_pipefd[1] = pipefd[1];
    }
    return ret;
}

int sys_pipe2_user(int* user_pipefd, int flags) {
    int pipefd[2];
    int ret = fs_pipe2(pipefd, flags);
    if (ret == 0) {
        user_pipefd[0] = pipefd[0];
        user_pipefd[1] = pipefd[1];
    }
    return ret;
}

void sys_ls_private(void) {
    sys_ls();
}
