#include <stdint.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pwd.h>
#include <grp.h>
#include <sys/utsname.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include "../include/syscall.h"

extern char **environ;

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#endif

#define WEAK_SYM __attribute__((weak))

static int64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint64_t ret;
    __asm__ volatile (
        "syscall\n"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return (int64_t)ret;
}

static int64_t syscall6(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = arg4;
    register uint64_t r8  __asm__("r8")  = arg5;
    register uint64_t r9  __asm__("r9")  = arg6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return (int64_t)ret;
}

WEAK_SYM char *optarg;
WEAK_SYM int optind = 1;
WEAK_SYM int opterr = 1;
WEAK_SYM int optopt;
WEAK_SYM int optreset;

static char *g_getopt_next;

static void getopt_reset_state(void) {
    optarg = 0;
    optopt = 0;
    g_getopt_next = 0;
    optind = 1;
    optreset = 0;
}

WEAK_SYM int getopt(int argc, char *const argv[], const char *optstring) {
    const char* spec;
    char* arg;
    char c;

    if (!argv || !optstring) return -1;
    if (optreset || optind <= 0) getopt_reset_state();
    optarg = 0;

    if (!g_getopt_next || *g_getopt_next == '\0') {
        if (optind >= argc) return -1;
        arg = argv[optind];
        if (!arg || arg[0] != '-' || arg[1] == '\0') return -1;
        if (arg[1] == '-' && arg[2] == '\0') {
            optind++;
            return -1;
        }
        g_getopt_next = arg + 1;
    }

    c = *g_getopt_next++;
    spec = optstring;
    if (*spec == '+' || *spec == '-' || *spec == ':') spec++;
    while (*spec && *spec != c) spec++;
    if (*spec != c) {
        optopt = (unsigned char)c;
        if (*g_getopt_next == '\0') {
            g_getopt_next = 0;
            optind++;
        }
        return '?';
    }

    if (spec[1] == ':') {
        if (*g_getopt_next != '\0') {
            optarg = g_getopt_next;
            g_getopt_next = 0;
            optind++;
        } else if (optind + 1 < argc) {
            optarg = argv[optind + 1];
            g_getopt_next = 0;
            optind += 2;
        } else {
            optopt = (unsigned char)c;
            g_getopt_next = 0;
            optind++;
            return (optstring[0] == ':') ? ':' : '?';
        }
        if (spec[2] == ':' && !optarg) optarg = 0;
    } else if (*g_getopt_next == '\0') {
        g_getopt_next = 0;
        optind++;
    }

    return (unsigned char)c;
}

static void build_exec_candidate(char* out, size_t out_size, const char* dir, const char* file, const char* suffix) {
    size_t di = 0;
    size_t fi = 0;
    size_t oi = 0;
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!dir || !file) return;
    while (dir[di] && oi + 1 < out_size) out[oi++] = dir[di++];
    if (oi > 0 && out[oi - 1] != '/' && oi + 1 < out_size) out[oi++] = '/';
    while (file[fi] && oi + 1 < out_size) out[oi++] = file[fi++];
    if (suffix) {
        fi = 0;
        while (suffix[fi] && oi + 1 < out_size) out[oi++] = suffix[fi++];
    }
    out[oi] = '\0';
}

static int simple_fnmatch_impl(const char* pat, const char* str) {
    if (!pat || !str) return FNM_NOMATCH;
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 0;
            while (*str) {
                if (simple_fnmatch_impl(pat, str) == 0) return 0;
                str++;
            }
            return FNM_NOMATCH;
        }
        if (*pat == '?') {
            if (!*str) return FNM_NOMATCH;
            pat++;
            str++;
            continue;
        }
        if (*pat != *str) return FNM_NOMATCH;
        pat++;
        str++;
    }
    return *str ? FNM_NOMATCH : 0;
}

// System call implementations (internal)

int _fork(void) { return (int)syscall(SYS_FORK, 0, 0, 0); }
int _execve(const char *pathname, char *const argv[], char *const envp[]) { return (int)syscall(SYS_EXECVE, (uint64_t)pathname, (uint64_t)argv, (uint64_t)envp); }
int _wait(int *wstatus) { return (int)syscall(SYS_WAIT4, (uint64_t)-1, (uint64_t)wstatus, 0); }
int _getpid(void) { return (int)syscall(SYS_GETPID, 0, 0, 0); }
void _exit(int status) { syscall(SYS_EXIT, (uint64_t)status, 0, 0); while(1); }
int _write(int fd, const void* buf, int count) { return (int)syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, (uint64_t)count); }
int _open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int ret = (int)syscall(SYS_OPEN, (uint64_t)path, (uint64_t)flags, (uint64_t)mode);
    if (ret == -1) errno = ENOENT;
    return ret;
}
WEAK_SYM int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int ret = (int)syscall6(SYS_OPENAT, (uint64_t)dirfd, (uint64_t)path, (uint64_t)flags, (uint64_t)mode, 0, 0);
    if (ret == -1) errno = ENOENT;
    return ret;
}
int _read(int fd, void* buf, int count) { return (int)syscall(SYS_READ, (uint64_t)fd, (uint64_t)buf, (uint64_t)count); }
int _close(int fd) { return (int)syscall(SYS_CLOSE, (uint64_t)fd, 0, 0); }
void* _sbrk(intptr_t increment) {
    uint64_t current_brk = (uint64_t)syscall(SYS_BRK, 0, 0, 0);
    if (increment == 0) return (void*)current_brk;
    uint64_t new_brk = (uint64_t)syscall(SYS_BRK, current_brk + increment, 0, 0);
    if (new_brk == current_brk && increment > 0) return (void*)-1;
    return (void*)current_brk;
}
struct kstat {
    uint64_t dev;
    uint64_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t rdev;
    int64_t size;
    int64_t atime_sec;
    int64_t mtime_sec;
    int64_t ctime_sec;
};

static void stat_from_kstat(struct stat* st, const struct kstat* kst) {
    if (!st || !kst) return;
    memset(st, 0, sizeof(*st));
    st->st_dev = (dev_t)kst->dev;
    st->st_ino = (ino_t)kst->ino;
    st->st_mode = kst->mode;
    st->st_nlink = kst->nlink;
    st->st_uid = kst->uid;
    st->st_gid = kst->gid;
    st->st_rdev = (dev_t)kst->rdev;
    st->st_size = kst->size;
    st->st_blksize = 512;
    st->st_blocks = (kst->size + 511) / 512;
    st->st_atim.tv_sec = kst->atime_sec;
    st->st_mtim.tv_sec = kst->mtime_sec;
    st->st_ctim.tv_sec = kst->ctime_sec;
}

int _fstat(int fd, struct stat *st) {
    struct kstat kst;
    int ret = (int)syscall(SYS_FSTAT, (uint64_t)fd, (uint64_t)&kst, 0);
    if (ret == 0) {
        stat_from_kstat(st, &kst);
    } else {
        errno = ENOENT;
    }
    return ret;
}

int _stat(const char *path, struct stat *st) {
    struct kstat kst;
    int ret = (int)syscall(SYS_STAT, (uint64_t)path, (uint64_t)&kst, 0);
    if (ret == 0) {
        stat_from_kstat(st, &kst);
    } else {
        errno = ENOENT;
    }
    return ret;
}

int _isatty(int fd) {
    struct stat st;
    if (_fstat(fd, &st) == 0) {
        return ((st.st_mode & 0170000) == 0020000) ? 1 : 0;
    }
    return 0;
}
int _lseek(int fd, int ptr, int dir) { return (int)syscall(SYS_LSEEK, (uint64_t)fd, (uint64_t)ptr, (uint64_t)dir); }
int _unlink(const char *path) { return (int)syscall(SYS_UNLINK, (uint64_t)path, 0, 0); }
int _rename(const char *oldpath, const char *newpath) { return (int)syscall(SYS_RENAME, (uint64_t)oldpath, (uint64_t)newpath, 0); }
int _pipe(int pipefd[2]) { return (int)syscall(SYS_PIPE, (uint64_t)pipefd, 0, 0); }
WEAK_SYM int pipe2(int pipefd[2], int flags) { return (int)syscall(SYS_PIPE2, (uint64_t)pipefd, (uint64_t)flags, 0); }
int _dup2(int oldfd, int newfd) { return (int)syscall(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd, 0); }
int chdir_sys(const char* path) { return (int)syscall(SYS_CHDIR, (uint64_t)path, 0, 0); }
int fchdir_sys(int fd) { return (int)syscall(SYS_FCHDIR, (uint64_t)fd, 0, 0); }
int mkdir_sys(const char* path, int mode) { return (int)syscall(SYS_MKDIR, (uint64_t)path, (uint64_t)mode, 0); }
WEAK_SYM int mkdirat(int dirfd, const char *path, mode_t mode) { return (int)syscall(SYS_MKDIRAT, (uint64_t)dirfd, (uint64_t)path, (uint64_t)mode); }
int rmdir_sys(const char* path) { return (int)syscall(SYS_RMDIR, (uint64_t)path, 0, 0); }
WEAK_SYM int unlinkat(int dirfd, const char *path, int flags) { return (int)syscall(SYS_UNLINKAT, (uint64_t)dirfd, (uint64_t)path, (uint64_t)flags); }
WEAK_SYM int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    struct kstat kst;
    int ret = (int)syscall6(SYS_FSTATAT, (uint64_t)dirfd, (uint64_t)path, (uint64_t)&kst, (uint64_t)flags, 0, 0);
    if (ret == 0) stat_from_kstat(st, &kst); else errno = ENOENT;
    return ret;
}
int getcwd_sys(char* buf, uint32_t size) { return (int)syscall(SYS_GETCWD, (uint64_t)buf, (uint64_t)size, 0); }
int _access(const char *path, int mode) {
    struct stat st;
    return _stat(path, &st);
}
int _kill(int pid, int sig) { return (int)syscall(SYS_KILL, (uint64_t)pid, (uint64_t)sig, 0); }

WEAK_SYM long sysconf(int name) {
    (void)name;
    return 4096;
}

WEAK_SYM int getpagesize(void) {
    return 4096;
}

WEAK_SYM mode_t umask(mode_t mask) {
    (void)mask;
    return 0;
}

WEAK_SYM int chmod(const char *path, mode_t mode) {
    return (int)syscall(SYS_CHMOD, (uint64_t)path, (uint64_t)mode, 0);
}

WEAK_SYM int rename(const char *oldpath, const char *newpath) {
    return _rename(oldpath, newpath);
}

WEAK_SYM int fcntl(int fd, int cmd, ...) {
    __builtin_va_list ap;
    uint64_t arg = 0;
    __builtin_va_start(ap, cmd);
    arg = __builtin_va_arg(ap, uint64_t);
    __builtin_va_end(ap);
    return (int)syscall(SYS_FCNTL, (uint64_t)fd, (uint64_t)cmd, arg);
}

WEAK_SYM int ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    switch (request) {
        case TIOCGWINSZ:
            {
                __builtin_va_list ap;
                struct winsize* ws;
                __builtin_va_start(ap, request);
                ws = __builtin_va_arg(ap, struct winsize*);
                __builtin_va_end(ap);
                if (!ws) {
                    errno = EINVAL;
                    return -1;
                }
                ws->ws_row = 25;
                ws->ws_col = 80;
                ws->ws_xpixel = 0;
                ws->ws_ypixel = 0;
                return 0;
            }
        case TIOCGPGRP:
            {
                __builtin_va_list ap;
                pid_t* pgrp;
                __builtin_va_start(ap, request);
                pgrp = __builtin_va_arg(ap, pid_t*);
                __builtin_va_end(ap);
                if (!pgrp) {
                    errno = EINVAL;
                    return -1;
                }
                *pgrp = (pid_t)syscall(ORTH_SYS_TCGETPGRP, (uint64_t)fd, 0, 0);
                return (*pgrp < 0) ? -1 : 0;
            }
        case TIOCSPGRP:
            {
                __builtin_va_list ap;
                pid_t* pgrp;
                __builtin_va_start(ap, request);
                pgrp = __builtin_va_arg(ap, pid_t*);
                __builtin_va_end(ap);
                if (!pgrp) {
                    errno = EINVAL;
                    return -1;
                }
                return (int)syscall(ORTH_SYS_TCSETPGRP, (uint64_t)fd, (uint64_t)(*pgrp), 0);
            }
        default:
            errno = ENOTTY;
            return -1;
    }
}

WEAK_SYM char *getcwd(char *buf, size_t size) {
    int ret;
    if (!buf || size == 0) {
        errno = EINVAL;
        return 0;
    }
    ret = getcwd_sys(buf, (uint32_t)size);
    if (ret < 0) {
        errno = ERANGE;
        return 0;
    }
    return buf;
}

WEAK_SYM int mkdir(const char *pathname, mode_t mode) {
    return mkdir_sys(pathname, (int)mode);
}

WEAK_SYM int mknod(const char *pathname, mode_t mode, dev_t dev) {
    (void)pathname;
    (void)mode;
    (void)dev;
    errno = ENOSYS;
    return -1;
}

WEAK_SYM int utime(const char *filename, const void *times) {
    (void)filename;
    (void)times;
    return 0;
}

WEAK_SYM int utimes(const char* path, const struct timeval times[2]) {
    (void)path;
    (void)times;
    return 0;
}

WEAK_SYM int chown(const char *path, uid_t owner, gid_t group) {
    (void)path;
    (void)owner;
    (void)group;
    return 0;
}

WEAK_SYM int link(const char *oldpath, const char *newpath) {
    (void)oldpath;
    (void)newpath;
    errno = EMLINK;
    return -1;
}

WEAK_SYM ssize_t readlink(const char* path, char* buf, size_t bufsiz) {
    const char* target = 0;
    size_t len = 0;
    if (!path || !buf || bufsiz == 0) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(path, "/dev/tty") == 0) {
        target = "/dev/tty";
    } else if (strcmp(path, "/dev/stdin") == 0 || strcmp(path, "/dev/fd/0") == 0) {
        target = "/dev/tty";
    } else if (strcmp(path, "/dev/stdout") == 0 || strcmp(path, "/dev/fd/1") == 0) {
        target = "/dev/tty";
    } else if (strcmp(path, "/dev/stderr") == 0 || strcmp(path, "/dev/fd/2") == 0) {
        target = "/dev/tty";
    } else {
        errno = EINVAL;
        return -1;
    }
    while (target[len] != '\0') len++;
    if (len > bufsiz) len = bufsiz;
    for (size_t i = 0; i < len; i++) buf[i] = target[i];
    return (ssize_t)len;
}

WEAK_SYM int rmdir(const char *pathname) {
    return rmdir_sys(pathname);
}

WEAK_SYM unsigned int sleep(unsigned int seconds) {
    (void)seconds;
    return 0;
}

WEAK_SYM int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    (void)timeout;
    if (!fds || nfds == 0) return 0;
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
    }
    return (int)nfds;
}

WEAK_SYM int getrlimit(int resource, struct rlimit* rlim) {
    (void)resource;
    if (!rlim) {
        errno = EINVAL;
        return -1;
    }
    rlim->rlim_cur = RLIM_INFINITY;
    rlim->rlim_max = RLIM_INFINITY;
    return 0;
}

WEAK_SYM int setrlimit(int resource, const struct rlimit* rlim) {
    (void)resource;
    (void)rlim;
    return 0;
}

WEAK_SYM ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
                        const struct sockaddr* dest_addr, socklen_t addrlen) {
    long ret = syscall6(SYS_SENDTO, (uint64_t)sockfd, (uint64_t)buf, (uint64_t)len,
                        (uint64_t)flags, (uint64_t)dest_addr, (uint64_t)addrlen);
    if (ret < 0) {
        errno = (ret == -11) ? EAGAIN : EINVAL;
        return -1;
    }
    return ret;
}

WEAK_SYM ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags,
                          struct sockaddr* src_addr, socklen_t* addrlen) {
    long ret = syscall6(SYS_RECVFROM, (uint64_t)sockfd, (uint64_t)buf, (uint64_t)len,
                        (uint64_t)flags, (uint64_t)src_addr, (uint64_t)addrlen);
    if (ret < 0) {
        errno = (ret == -11) ? EAGAIN : EINVAL;
        return -1;
    }
    return ret;
}

WEAK_SYM int socket(int domain, int type, int protocol) {
    long ret = syscall(SYS_SOCKET, (uint64_t)domain, (uint64_t)type, (uint64_t)protocol);
    if (ret < 0) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return (int)ret;
}

WEAK_SYM int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    long ret = syscall(SYS_CONNECT, (uint64_t)sockfd, (uint64_t)addr, (uint64_t)addrlen);
    if (ret < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

WEAK_SYM int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    long ret = syscall(SYS_ACCEPT, (uint64_t)sockfd, (uint64_t)addr, (uint64_t)addrlen);
    if (ret < 0) {
        errno = (ret == -11) ? EAGAIN : EINVAL;
        return -1;
    }
    return (int)ret;
}

WEAK_SYM int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    long ret = syscall(SYS_BIND, (uint64_t)sockfd, (uint64_t)addr, (uint64_t)addrlen);
    if (ret < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

WEAK_SYM int listen(int sockfd, int backlog) {
    long ret = syscall(SYS_LISTEN, (uint64_t)sockfd, (uint64_t)backlog, 0);
    if (ret < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

WEAK_SYM int getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    long ret = syscall(SYS_GETSOCKNAME, (uint64_t)sockfd, (uint64_t)addr, (uint64_t)addrlen);
    if (ret < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

WEAK_SYM int getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    long ret = syscall(SYS_GETPEERNAME, (uint64_t)sockfd, (uint64_t)addr, (uint64_t)addrlen);
    if (ret < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

WEAK_SYM int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
    long ret = syscall6(SYS_SETSOCKOPT, (uint64_t)sockfd, (uint64_t)level, (uint64_t)optname,
                        (uint64_t)optval, (uint64_t)optlen, 0);
    if (ret < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

WEAK_SYM int shutdown(int sockfd, int how) {
    long ret = syscall(SYS_SHUTDOWN, (uint64_t)sockfd, (uint64_t)how, 0);
    if (ret < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

WEAK_SYM ssize_t send(int sockfd, const void* buf, size_t len, int flags) {
    return sendto(sockfd, buf, len, flags, 0, 0);
}

WEAK_SYM ssize_t recv(int sockfd, void* buf, size_t len, int flags) {
    return recvfrom(sockfd, buf, len, flags, 0, 0);
}

WEAK_SYM unsigned int alarm(unsigned int seconds) {
    (void)seconds;
    return 0;
}

WEAK_SYM int execvp(const char *file, char *const argv[]) {
    char path_copy[256];
    char candidate[256];
    char* path_env;
    char* dir;
    struct stat st;

    if (!file || !file[0]) {
        errno = ENOENT;
        return -1;
    }

    if (strchr(file, '/')) {
        _execve(file, argv, environ);
        if (!strstr(file, ".elf")) {
            build_exec_candidate(candidate, sizeof(candidate), "", file, ".elf");
            _execve(candidate, argv, environ);
        }
        errno = ENOENT;
        return -1;
    }

    path_env = getenv("PATH");
    if (!path_env || !path_env[0]) path_env = "/:/bin:/usr/bin:/boot";
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    dir = strtok(path_copy, ":");
    while (dir) {
        build_exec_candidate(candidate, sizeof(candidate), dir, file, NULL);
        if (stat(candidate, &st) == 0) {
            _execve(candidate, argv, environ);
        }
        if (!strstr(file, ".elf")) {
            build_exec_candidate(candidate, sizeof(candidate), dir, file, ".elf");
            if (stat(candidate, &st) == 0) {
                _execve(candidate, argv, environ);
            }
        }
        dir = strtok(NULL, ":");
    }

    errno = ENOENT;
    return -1;
}

WEAK_SYM int execv(const char *path, char *const argv[]) {
    return _execve(path, argv, environ);
}

WEAK_SYM int chdir(const char *path) {
    if (chdir_sys(path) < 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

WEAK_SYM int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (tv) {
        tv->tv_sec = 0;
        tv->tv_usec = 0;
    }
    return 0;
}

WEAK_SYM int tcgetattr(int fd, struct termios* tio) {
    if (!tio) {
        errno = EINVAL;
        return -1;
    }
    return (int)syscall(ORTH_SYS_TCGETATTR, (uint64_t)fd, (uint64_t)tio, 0);
}

WEAK_SYM int tcsetattr(int fd, int optional_actions, const struct termios* tio) {
    return (int)syscall(ORTH_SYS_TCSETATTR, (uint64_t)fd, (uint64_t)optional_actions, (uint64_t)tio);
}

WEAK_SYM int tcflush(int fd, int queue_selector) {
    (void)fd;
    (void)queue_selector;
    return 0;
}

int h_errno;

WEAK_SYM char* hstrerror(int err) {
    (void)err;
    return "resolver unavailable";
}

WEAK_SYM struct netent* getnetbyname(const char* name) {
    (void)name;
    return NULL;
}

WEAK_SYM struct servent* getservbyname(const char* name, const char* proto) {
    (void)name;
    (void)proto;
    return NULL;
}

WEAK_SYM struct netent* getnetbyaddr(unsigned long net, int type) {
    (void)net;
    (void)type;
    return NULL;
}

WEAK_SYM int uname(struct utsname* buf) {
    if (!buf) {
        errno = EINVAL;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    strcpy(buf->sysname, "OrthOS");
    strcpy(buf->nodename, "orthos");
    strcpy(buf->release, "0.1");
    strcpy(buf->version, "OrthOS");
    strcpy(buf->machine, "x86_64");
    return 0;
}

WEAK_SYM int clearenv(void) {
    if (environ) {
        environ[0] = NULL;
    }
    return 0;
}

WEAK_SYM int getnameinfo(const struct sockaddr* addr, socklen_t addrlen,
                         char* host, socklen_t hostlen,
                         char* serv, socklen_t servlen, int flags) {
    (void)addr;
    (void)addrlen;
    (void)flags;
    if (host && hostlen) host[0] = '\0';
    if (serv && servlen) serv[0] = '\0';
    errno = ENOSYS;
    return -1;
}

WEAK_SYM int fnmatch(const char* pattern, const char* string, int flags) {
    (void)flags;
    return simple_fnmatch_impl(pattern, string);
}

WEAK_SYM clock_t times(struct tms* buf) {
    if (buf) memset(buf, 0, sizeof(*buf));
    return 0;
}

WEAK_SYM struct passwd* getpwnam(const char* name) {
    static struct passwd pwd;
    static char home[] = "/";
    static char shell[] = "/bin/sh";
    if (!name) return NULL;
    pwd.pw_name = (char*)name;
    pwd.pw_passwd = (char*)"x";
    pwd.pw_uid = 0;
    pwd.pw_gid = 0;
    pwd.pw_gecos = (char*)"root";
    pwd.pw_dir = home;
    pwd.pw_shell = shell;
    return &pwd;
}

WEAK_SYM struct passwd* getpwuid(uid_t uid) {
    static char root_name[] = "root";
    (void)uid;
    return getpwnam(root_name);
}

WEAK_SYM struct group* getgrgid(gid_t gid) {
    static struct group grp;
    static char group_name[] = "root";
    static char* members[] = { NULL };
    (void)gid;
    grp.gr_name = group_name;
    grp.gr_passwd = (char*)"x";
    grp.gr_gid = 0;
    grp.gr_mem = members;
    return &grp;
}

WEAK_SYM int getgroups(int size, gid_t list[]) {
    if (size > 0 && list) list[0] = 0;
    return 1;
}

WEAK_SYM uid_t getuid(void) { return 0; }
WEAK_SYM uid_t geteuid(void) { return 0; }
WEAK_SYM gid_t getgid(void) { return 0; }
WEAK_SYM gid_t getegid(void) { return 0; }
WEAK_SYM pid_t getppid(void) { return 1; }
WEAK_SYM int setuid(uid_t uid) { (void)uid; return 0; }
WEAK_SYM int seteuid(uid_t uid) { (void)uid; return 0; }
WEAK_SYM int setgid(gid_t gid) { (void)gid; return 0; }
WEAK_SYM int setegid(gid_t gid) { (void)gid; return 0; }
WEAK_SYM int fchdir(int fd) { return fchdir_sys(fd); }
WEAK_SYM int chroot(const char* path) { (void)path; errno = ENOSYS; return -1; }
WEAK_SYM int ttyname_r(int fd, char* buf, size_t buflen) {
    (void)fd;
    if (!buf || buflen < 9) { errno = ERANGE; return ERANGE; }
    strcpy(buf, "/dev/tty");
    return 0;
}
WEAK_SYM pid_t vfork(void) { return _fork(); }
WEAK_SYM int sigsuspend(const sigset_t* mask) { (void)mask; errno = EINTR; return -1; }

WEAK_SYM pid_t getpgrp(void) {
    return (pid_t)syscall(SYS_GETPGRP, 0, 0, 0);
}

WEAK_SYM int setpgid(pid_t pid, pid_t pgid) {
    return (int)syscall(SYS_SETPGID, (uint64_t)pid, (uint64_t)pgid, 0);
}

WEAK_SYM pid_t setsid(void) {
    return (pid_t)syscall(SYS_SETSID, 0, 0, 0);
}

WEAK_SYM pid_t tcgetpgrp(int fd) {
    return (pid_t)syscall(ORTH_SYS_TCGETPGRP, (uint64_t)fd, 0, 0);
}

WEAK_SYM int tcsetpgrp(int fd, pid_t pgrp_id) {
    return (int)syscall(ORTH_SYS_TCSETPGRP, (uint64_t)fd, (uint64_t)pgrp_id, 0);
}

WEAK_SYM int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    struct orth_sigaction in_act;
    struct orth_sigaction out_act;
    int ret;
    struct orth_sigaction* in_ptr = 0;
    struct orth_sigaction* out_ptr = 0;
    if (act) {
        in_act.sa_handler = (uint64_t)(uintptr_t)act->sa_handler;
        in_act.sa_mask = (uint64_t)act->sa_mask;
        in_act.sa_flags = (uint32_t)act->sa_flags;
        in_act.reserved = 0;
        in_ptr = &in_act;
    }
    if (oldact) {
        out_ptr = &out_act;
    }
    ret = (int)syscall(ORTH_SYS_SIGACTION, (uint64_t)sig, (uint64_t)in_ptr, (uint64_t)out_ptr);
    if (ret == 0 && oldact) {
        oldact->sa_handler = (_sig_func_ptr)(uintptr_t)out_act.sa_handler;
        oldact->sa_mask = (sigset_t)out_act.sa_mask;
        oldact->sa_flags = (int)out_act.sa_flags;
    }
    return ret;
}

WEAK_SYM int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    return (int)syscall(ORTH_SYS_SIGPROCMASK, (uint64_t)how, (uint64_t)set, (uint64_t)oldset);
}

WEAK_SYM int sigpending(sigset_t* set) {
    return (int)syscall(ORTH_SYS_SIGPENDING, (uint64_t)set, 0, 0);
}

_sig_func_ptr __wrap_signal(int sig, _sig_func_ptr handler) {
    struct sigaction act;
    struct sigaction oldact;
    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = 0;
    if (sigaction(sig, &act, &oldact) < 0) {
        return SIG_ERR;
    }
    return oldact.sa_handler;
}

WEAK_SYM char *getwd(char *buf) {
    return getcwd(buf, 2);
}

// Aliases for both underscore and non-underscore versions to satisfy Newlib and apps
int fork(void) { return _fork(); }
int execve(const char *path, char *const argv[], char *const envp[]) { return _execve(path, argv, envp); }
int write(int fd, const void* buf, int count) { return _write(fd, buf, count); }
int read(int fd, void* buf, int count) { return _read(fd, buf, count); }
int open(const char* path, int flags, ...) { return _open(path, flags); }
int close(int fd) { return _close(fd); }
void* sbrk(intptr_t inc) { return _sbrk(inc); }
int getpid(void) { return _getpid(); }
int waitpid(int pid, int* wstatus, int options) { return (int)syscall(SYS_WAIT4, (uint64_t)pid, (uint64_t)wstatus, (uint64_t)options); }
int fstat(int fd, struct stat *st) { return _fstat(fd, st); }
int stat(const char *path, struct stat *st) { return _stat(path, st); }
int lstat(const char *path, struct stat *st) { return _stat(path, st); }
int access(const char *path, int mode) { return _access(path, mode); }
int isatty(int fd) { return _isatty(fd); }
int lseek(int fd, int ptr, int dir) { return _lseek(fd, ptr, dir); }
int unlink(const char *path) { return _unlink(path); }
int pipe(int pipefd[2]) { return _pipe(pipefd); }
int dup2(int oldfd, int newfd) { return _dup2(oldfd, newfd); }
int dup(int fd) { return fcntl(fd, F_DUPFD, 0); }
int kill(int pid, int sig) { return _kill(pid, sig); }
int wait(int *wstatus) { return _wait(wstatus); }

void* mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    return (void*)syscall6(SYS_MMAP, (uint64_t)addr, (uint64_t)length, (uint64_t)prot,
                           (uint64_t)flags, (uint64_t)fd, (uint64_t)offset);
}

int munmap(void *addr, size_t length) {
    return (int)syscall(SYS_MUNMAP, (uint64_t)addr, (uint64_t)length, 0);
}

int get_video_info(struct video_info* info) {
    return (int)syscall(ORTH_SYS_GET_VIDEO_INFO, (uint64_t)info, 0, 0);
}

uint64_t map_framebuffer(void) {
    return (uint64_t)syscall(ORTH_SYS_MAP_FRAMEBUFFER, 0, 0, 0);
}

uint64_t get_ticks_ms(void) {
    return (uint64_t)syscall(ORTH_SYS_GET_TICKS_MS, 0, 0, 0);
}

int sleep_ms(uint64_t ms) {
    return (int)syscall(ORTH_SYS_SLEEP_MS, ms, 0, 0);
}

int get_key_event(struct key_event* ev) {
    return (int)syscall(ORTH_SYS_GET_KEY_EVENT, (uint64_t)ev, 0, 0);
}

int sound_on(uint32_t freq_hz) {
    return (int)syscall(ORTH_SYS_SOUND_ON, (uint64_t)freq_hz, 0, 0);
}

int sound_off(void) {
    return (int)syscall(ORTH_SYS_SOUND_OFF, 0, 0, 0);
}

int sound_pcm_u8(const uint8_t* samples, uint32_t count, uint32_t sample_rate) {
    return (int)syscall(ORTH_SYS_SOUND_PCM_U8, (uint64_t)samples, (uint64_t)count, (uint64_t)sample_rate);
}

int usb_info(void) {
    return (int)syscall(ORTH_SYS_USB_INFO, 0, 0, 0);
}

int usb_read_block_sys(uint32_t lba, void* buf, uint32_t count) {
    return (int)syscall(ORTH_SYS_USB_READ_BLOCK, (uint64_t)lba, (uint64_t)buf, (uint64_t)count);
}

int mount_usb_root(const char* path) {
    return (int)syscall(ORTH_SYS_MOUNT_USB_ROOT, (uint64_t)path, 0, 0);
}

int mount_module_root(void) {
    return (int)syscall(ORTH_SYS_MOUNT_MODULE_ROOT, 0, 0, 0);
}

int get_mount_status(char* buf, uint32_t size) {
    return (int)syscall(ORTH_SYS_GET_MOUNT_STATUS, (uint64_t)buf, (uint64_t)size, 0);
}

int getdents_sys(int fd, struct orth_dirent* dirp, uint32_t count) {
    return (int)syscall(SYS_GETDENTS, (uint64_t)fd, (uint64_t)dirp, (uint64_t)count);
}

WEAK_SYM DIR *opendir(const char *path) {
    DIR* dir;
    int fd;
    if (!path) {
        errno = ENOENT;
        return 0;
    }
    fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return 0;
    dir = (DIR*)malloc(sizeof(DIR));
    if (!dir) {
        close(fd);
        errno = ENOMEM;
        return 0;
    }
    dir->dd_fd = fd;
    dir->dd_loc = 0;
    dir->dd_size = 0;
    dir->dd_dirent.d_name[0] = '\0';
    return dir;
}

WEAK_SYM DIR *fdopendir(int fd) {
    DIR* dir;
    if (fd < 0) {
        errno = EBADF;
        return 0;
    }
    dir = (DIR*)malloc(sizeof(DIR));
    if (!dir) {
        errno = ENOMEM;
        return 0;
    }
    dir->dd_fd = fd;
    dir->dd_loc = 0;
    dir->dd_size = 0;
    dir->dd_dirent.d_name[0] = '\0';
    return dir;
}

WEAK_SYM struct dirent *readdir(DIR *dir) {
    struct orth_dirent ent;
    int n;
    if (!dir) {
        errno = EBADF;
        return 0;
    }
    n = getdents_sys(dir->dd_fd, &ent, sizeof(ent));
    if (n <= 0) return 0;
    dir->dd_loc += n;
    dir->dd_size = n;
    dir->dd_dirent.d_ino = 0;
    dir->dd_dirent.d_off = dir->dd_loc;
    dir->dd_dirent.d_reclen = sizeof(struct dirent);
    dir->dd_dirent.d_type = (ent.mode == S_IFDIR) ? DT_DIR : DT_REG;
    {
        size_t i = 0;
        for (; i < MAXNAMLEN && ent.name[i]; i++) dir->dd_dirent.d_name[i] = ent.name[i];
        dir->dd_dirent.d_name[i] = '\0';
    }
    return &dir->dd_dirent;
}

WEAK_SYM int closedir(DIR *dir) {
    int ret;
    if (!dir) {
        errno = EBADF;
        return -1;
    }
    ret = close(dir->dd_fd);
    free(dir);
    return ret;
}

WEAK_SYM void rewinddir(DIR *dir) {
    if (!dir) return;
    lseek(dir->dd_fd, 0, SEEK_SET);
    dir->dd_loc = 0;
    dir->dd_size = 0;
}

WEAK_SYM int dirfd(DIR *dir) {
    if (!dir) {
        errno = EBADF;
        return -1;
    }
    return dir->dd_fd;
}

static int simple_match_component(const char* pattern, const char* text) {
    if (!pattern || !text) return 0;
    if (*pattern == '\0') return *text == '\0';
    if (*pattern == '*') {
        do {
            if (simple_match_component(pattern + 1, text)) return 1;
        } while (*text++ != '\0');
        return 0;
    }
    if (*pattern == '?') {
        return (*text != '\0') && simple_match_component(pattern + 1, text + 1);
    }
    if (*pattern != *text) return 0;
    return simple_match_component(pattern + 1, text + 1);
}

WEAK_SYM int scandir(const char *path, struct dirent ***namelist,
                     int (*filter)(const struct dirent *),
                     int (*compar)(const struct dirent **, const struct dirent **)) {
    DIR* dir;
    struct dirent** list = 0;
    int count = 0;
    int cap = 0;
    struct dirent* ent;
    (void)compar;
    if (!path || !namelist) {
        errno = EINVAL;
        return -1;
    }
    dir = opendir(path);
    if (!dir) return -1;
    while ((ent = readdir(dir)) != 0) {
        struct dirent* copy;
        struct dirent** next;
        if (filter && !filter(ent)) continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 8;
            next = (struct dirent**)realloc(list, (size_t)cap * sizeof(*list));
            if (!next) {
                closedir(dir);
                for (int i = 0; i < count; i++) free(list[i]);
                free(list);
                errno = ENOMEM;
                return -1;
            }
            list = next;
        }
        copy = (struct dirent*)malloc(sizeof(struct dirent));
        if (!copy) {
            closedir(dir);
            for (int i = 0; i < count; i++) free(list[i]);
            free(list);
            errno = ENOMEM;
            return -1;
        }
        memcpy(copy, ent, sizeof(struct dirent));
        list[count++] = copy;
    }
    closedir(dir);
    *namelist = list;
    return count;
}

WEAK_SYM int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob) {
    const char* slash;
    char dirbuf[256];
    const char* dirpath;
    const char* basepat;
    struct dirent** namelist = 0;
    char** matches = 0;
    int count = 0;
    int cap = 0;
    int dircount;
    (void)flags;
    (void)errfunc;
    if (!pattern || !pglob) {
        errno = EINVAL;
        return GLOB_ABEND;
    }
    slash = strrchr(pattern, '/');
    if (!slash) {
        dirpath = "/";
        basepat = pattern;
    } else if (slash == pattern) {
        dirpath = "/";
        basepat = slash + 1;
    } else {
        size_t len = (size_t)(slash - pattern);
        if (len >= sizeof(dirbuf)) len = sizeof(dirbuf) - 1;
        memcpy(dirbuf, pattern, len);
        dirbuf[len] = '\0';
        dirpath = dirbuf;
        basepat = slash + 1;
    }
    dircount = scandir(dirpath, &namelist, 0, 0);
    if (dircount < 0) return GLOB_ABEND;
    for (int di = 0; di < dircount; di++) {
        struct dirent* ent = namelist[di];
        char* path;
        char** next;
        size_t need;
        if (!simple_match_component(basepat, ent->d_name)) continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 8;
            next = (char**)realloc(matches, (size_t)cap * sizeof(*matches));
            if (!next) {
                for (int i = 0; i < dircount; i++) free(namelist[i]);
                free(namelist);
                for (int i = 0; i < count; i++) free(matches[i]);
                free(matches);
                return GLOB_NOSPACE;
            }
            matches = next;
        }
        need = strlen(dirpath) + 1 + strlen(ent->d_name) + 2;
        path = (char*)malloc(need);
        if (!path) {
            for (int i = 0; i < dircount; i++) free(namelist[i]);
            free(namelist);
            for (int i = 0; i < count; i++) free(matches[i]);
            free(matches);
            return GLOB_NOSPACE;
        }
        if (strcmp(dirpath, "/") == 0) {
            path[0] = '/';
            path[1] = '\0';
        } else {
            strcpy(path, dirpath);
            strcat(path, "/");
        }
        strcat(path, ent->d_name);
        if (ent->d_type == DT_DIR) strcat(path, "/");
        matches[count++] = path;
    }
    for (int i = 0; i < dircount; i++) free(namelist[i]);
    free(namelist);
    if (count == 0) {
        free(matches);
        return GLOB_NOMATCH;
    }
    pglob->gl_pathc = count;
    pglob->gl_matchc = count;
    pglob->gl_pathv = matches;
    return 0;
}

WEAK_SYM void globfree(glob_t *pglob) {
    if (!pglob || !pglob->gl_pathv) return;
    for (int i = 0; i < pglob->gl_pathc; i++) free(pglob->gl_pathv[i]);
    free(pglob->gl_pathv);
    pglob->gl_pathv = 0;
    pglob->gl_pathc = 0;
    pglob->gl_matchc = 0;
}
