#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "sys_internal.h"
#include "gdt.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"
#include "fs.h"
#include "limine.h"
#include "lapic.h"
#include "sound.h"
#include "usb.h"
#include "net_socket.h"
#include "lwip_port.h"
#include "spinlock.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084
#define ORTH_TCGETS    0x5401
#define ORTH_TCSETS    0x5402
#define ORTH_TIOCGPGRP 0x540F
#define ORTH_TIOCSPGRP 0x5410
#define ORTH_TIOCGWINSZ 0x5413

extern void syscall_entry(void);
extern struct task* task_list;
extern int64_t sys_write(int fd, const void* buf, size_t count);
extern int64_t sys_read(int fd, void* buf, size_t count);
extern int sys_close(int fd);
extern void puts(const char *s);
extern void puthex(uint64_t v);

#ifndef ORTHOX_MEM_TRACE
#define ORTHOX_MEM_TRACE 0
#endif

void sys_brk_init(uint64_t initial_break) {
    (void)initial_break;
}

struct syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = val >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline uint64_t rdtsc_u64(void) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#if ORTHOX_MEM_TRACE
static int kernel_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}
#endif

#if ORTHOX_MEM_PROGRESS
static void trace_progress_bump(struct task* current, uint64_t* counter) {
    if (!current || !current->trace_progress || !counter) return;
    (*counter)++;
}

static void trace_progress_bump_syscall(struct task* current, uint64_t syscall_no, uint64_t arg2) {
    if (!current || !current->trace_progress) return;
    switch (syscall_no) {
        case SYS_READ:
        case SYS_READV:
            current->trace_read_calls++;
            if (syscall_no == SYS_READ) current->trace_read_bytes += arg2;
            break;
        case SYS_WRITE:
        case SYS_WRITEV:
            current->trace_write_calls++;
            if (syscall_no == SYS_WRITE) {
                current->trace_write_bytes += arg2;
                if (arg2 > current->trace_write_max) current->trace_write_max = arg2;
            }
            break;
        case SYS_OPEN:
        case SYS_OPENAT:
            current->trace_open_calls++;
            break;
        case SYS_CLOSE:
            current->trace_close_calls++;
            break;
        case SYS_STAT:
        case SYS_LSTAT:
        case SYS_FSTATAT:
        case SYS_ACCESS:
        case SYS_FACCESSAT:
            current->trace_stat_calls++;
            break;
        case SYS_FSTAT:
            current->trace_fstat_calls++;
            break;
        case SYS_LSEEK:
            current->trace_lseek_calls++;
            break;
        case SYS_IOCTL:
            current->trace_ioctl_calls++;
            break;
        case SYS_CLOCK_GETTIME:
            current->trace_clock_calls++;
            break;
        case SYS_GETTIMEOFDAY:
            current->trace_gettimeofday_calls++;
            break;
        default:
            break;
    }
}
#endif

#if ORTHOX_MEM_TRACE
static int memtrace_current_enabled(void) {
    struct task* current = get_current_task();
    return current && kernel_streq(current->comm, "cc1");
}

static void memtrace_prefix(const char* tag) {
    struct task* current = get_current_task();
    puts("[memtrace] ");
    puts(tag);
    puts(" pid=0x"); puthex(current ? (uint64_t)current->pid : 0);
    puts(" brk=0x"); puthex(current ? current->heap_break : 0);
    puts(" mmap_end=0x"); puthex(current ? current->mmap_end : 0);
    puts(" pmm_alloc=0x"); puthex(pmm_get_allocated_pages());
    puts(" pmm_free=0x"); puthex(pmm_get_free_pages());
}
#endif

static void memtrace_mmap(const char* tag, uint64_t addr, uint64_t length, uint64_t result,
                          int prot, int flags, int fd, uint64_t pages) {
#if ORTHOX_MEM_TRACE
    if (!memtrace_current_enabled()) return;
    memtrace_prefix(tag);
    puts(" addr=0x"); puthex(addr);
    puts(" len=0x"); puthex(length);
    puts(" result=0x"); puthex(result);
    puts(" prot=0x"); puthex((uint64_t)prot);
    puts(" flags=0x"); puthex((uint64_t)flags);
    puts(" fd=0x"); puthex((uint64_t)(int64_t)fd);
    puts(" pages=0x"); puthex(pages);
    puts("\r\n");
#else
    (void)tag; (void)addr; (void)length; (void)result; (void)prot; (void)flags; (void)fd; (void)pages;
#endif
}

static uint64_t align_up_page(uint64_t v) {
    return (v + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static inline void cpuid_leaf(uint32_t leaf, uint32_t subleaf,
    uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    __asm__ volatile("cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(leaf), "c"(subleaf));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

static int cpu_has_rdrand(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    cpuid_leaf(1, 0, &eax, &ebx, &ecx, &edx);
    (void)eax;
    (void)ebx;
    (void)edx;
    return (ecx & (1U << 30)) != 0;
}

static int rdrand_u64(uint64_t* out) {
    unsigned char ok;
    uint64_t value;
    if (!out) return 0;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
    if (!ok) return 0;
    *out = value;
    return 1;
}

static int64_t sys_getrandom(void* buf, size_t len, unsigned flags) {
    uint8_t* out = (uint8_t*)buf;
    static uint64_t fallback_state = 0;
    uint64_t mix = 0;
    size_t off = 0;
    (void)flags;
    if (!out) return -1;
    if (fallback_state == 0) {
        fallback_state = rdtsc_u64() ^ (lapic_get_ticks_ms() << 17) ^ 0x9E3779B97F4A7C15ULL;
    }
    while (off < len) {
        uint64_t word = 0;
        size_t take;
        if (cpu_has_rdrand() && rdrand_u64(&word)) {
            mix ^= word;
        } else {
            fallback_state ^= fallback_state >> 12;
            fallback_state ^= fallback_state << 25;
            fallback_state ^= fallback_state >> 27;
            mix ^= fallback_state * 0x2545F4914F6CDD1DULL;
        }
        mix ^= rdtsc_u64();
        mix ^= lapic_get_ticks_ms() << 9;
        mix ^= (uint64_t)(uintptr_t)get_current_task();
        word = mix;
        take = len - off;
        if (take > sizeof(word)) take = sizeof(word);
        for (size_t i = 0; i < take; i++) {
            out[off + i] = (uint8_t)(word >> (i * 8));
        }
        off += take;
    }
    return (int64_t)len;
}

int64_t sys_write_serial(const char* buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (buf[i] == '\n') {
            __asm__ volatile("outb %b0, %w1" : : "a"((uint8_t)'\r'), "Nd"((uint16_t)0x3f8));
        }
        __asm__ volatile("outb %b0, %w1" : : "a"((uint8_t)buf[i]), "Nd"((uint16_t)0x3f8));
    }
    return (int64_t)count;
}

static int sys_usb_read_block(uint32_t lba, void* user_buf, uint32_t count) {
    uint8_t sector_buf[4096];
    uint8_t* dst = (uint8_t*)user_buf;
    uint32_t bytes;
    if (!dst || count == 0) return -1;
    if (count > 8) return -1;
    if (usb_read_block(lba, sector_buf, count) < 0) return -1;
    bytes = count * 512U;
    for (uint32_t i = 0; i < bytes; i++) {
        dst[i] = sector_buf[i];
    }
    return 0;
}

extern volatile struct limine_framebuffer_request framebuffer_request;

static int sys_get_video_info(struct video_info* info) {
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count == 0) {
        return -1;
    }
    struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
    info->width = fb->width;
    info->height = fb->height;
    info->pitch = fb->pitch;
    info->bpp = fb->bpp;
    return 0;
}

static uint64_t sys_map_framebuffer(void) {
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count == 0) {
        return 0;
    }
    struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
    struct task* current = get_current_task();
    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(current->ctx.cr3);
    uint64_t vaddr = 0x1000000000ULL;
    uint64_t paddr = (uint64_t)fb->address;
    if (paddr >= g_hhdm_offset) paddr -= g_hhdm_offset;
    uint64_t size = fb->pitch * fb->height;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    vmm_map_range(pml4, vaddr, paddr, size, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    return vaddr;
}

static uint64_t sys_get_ticks_ms(void) {
    return lapic_get_ticks_ms();
}

static int sys_set_fork_spread(int enabled) {
    return task_set_fork_spread(enabled);
}

static int sys_get_fork_spread(void) {
    return task_get_fork_spread();
}

extern int kb_get_event(struct key_event* ev);

static int sys_get_key_event(struct key_event* ev) {
    if (!ev) return 0;
    return kb_get_event(ev);
}

static int sys_sound_on(uint32_t freq_hz) {
    sound_beep_start(freq_hz);
    return 0;
}

static int sys_sound_off(void) {
    sound_beep_stop();
    return 0;
}

static int sys_sound_pcm_u8(const uint8_t* samples, uint32_t count, uint32_t sample_rate) {
    return sound_pcm_play_u8(samples, count, sample_rate);
}

void syscall_init_cpu(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);
    uint64_t star = (0x10ULL << 48) | (0x08ULL << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);
}

void syscall_init(void) {
    syscall_init_cpu();
}

static struct orth_termios g_console_termios = {
    .c_iflag = 0x00000002u,
    .c_oflag = 0x00000001u,
    .c_cflag = 0,
    .c_lflag = 0x00000001u | 0x00000002u | 0x00000008u,
    .c_cc = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26 },
    .c_ispeed = 115200,
    .c_ospeed = 115200,
};

static int sys_tcgetattr(int fd, struct orth_termios* tio) {
    (void)fd;
    if (!tio) return -1;
    *tio = g_console_termios;
    return 0;
}

static int sys_tcsetattr(int fd, int optional_actions, const struct orth_termios* tio) {
    (void)fd;
    (void)optional_actions;
    if (!tio) return -1;
    g_console_termios = *tio;
    return 0;
}

struct orth_iovec {
    const void* iov_base;
    size_t iov_len;
};

struct orth_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static int sys_ioctl(int fd, unsigned long request, uint64_t arg) {
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

static int64_t sys_writev(int fd, const struct orth_iovec* iov, int iovcnt) {
    int64_t total = 0;
    if (!iov || iovcnt < 0) return -1;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = sys_write(fd, iov[i].iov_base, iov[i].iov_len);
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

static int64_t sys_readv(int fd, const struct orth_iovec* iov, int iovcnt) {
    int64_t total = 0;
    if (!iov || iovcnt < 0) return -1;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = sys_read(fd, (void*)iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : rc;
        total += rc;
        if ((size_t)rc != iov[i].iov_len) break;
    }
    return total;
}

extern int task_fork(struct syscall_frame* frame);
extern int task_execve(struct syscall_frame* frame, const char* path, char* const argv[], char* const envp[]);
extern int64_t sys_write(int fd, const void* buf, size_t count);
extern int64_t sys_read(int fd, void* buf, size_t count);
extern int sys_open(const char* path, int flags, int mode);
extern int sys_openat(int dirfd, const char* path, int flags, int mode);
extern int sys_ftruncate(int fd, uint64_t length);
extern int sys_truncate(const char* path, uint64_t length);
extern int sys_close(int fd);
extern void sys_ls(void);
extern int sys_fstat(int fd, struct kstat* st);
extern int sys_stat(const char* path, struct kstat* st);
extern int sys_fstatat(int dirfd, const char* path, struct kstat* st, int flags);
extern int sys_access(const char* path, int mode);
extern int sys_faccessat(int dirfd, const char* path, int mode, int flags);
extern int64_t sys_readlink(const char* path, char* buf, size_t bufsiz);
extern int64_t sys_readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz);
extern int64_t sys_lseek(int fd, int64_t offset, int whence);
extern int sys_unlink(const char* path);
extern int sys_unlinkat(int dirfd, const char* path, int flags);
extern int sys_rename(const char* oldpath, const char* newpath);
extern int sys_chmod(const char* path, uint32_t mode);
extern int sys_chdir(const char* path);
extern int sys_getcwd(char* buf, size_t size);
extern int sys_getdents(int fd, struct orth_dirent* dirp, size_t count);
extern int sys_getdents64(int fd, void* dirp, size_t count);
extern int sys_fcntl(int fd, int cmd, uint64_t arg);
extern int sys_pipe(int pipefd[2]);
extern int sys_pipe2(int pipefd[2], int flags);
extern int sys_dup2(int oldfd, int newfd);
extern int sys_socket(int domain, int type, int protocol);
extern int sys_connect(int fd, const void* addr, uint32_t addrlen);
extern int sys_accept(int fd, void* addr, uint32_t* addrlen);
extern int sys_bind(int fd, const void* addr, uint32_t addrlen);
extern int sys_listen(int fd, int backlog);
extern int sys_setsockopt(int fd, int level, int optname, const void* optval, uint32_t optlen);
extern int sys_getsockname(int fd, void* addr, uint32_t* addrlen);
extern int sys_getpeername(int fd, void* addr, uint32_t* addrlen);
extern int sys_shutdown(int fd, int how);
extern int64_t sys_sendto(int fd, const void* buf, size_t len, int flags, const void* dest_addr, uint32_t addrlen);
extern int64_t sys_recvfrom(int fd, void* buf, size_t len, int flags, void* src_addr, uint32_t* addrlen);
extern int sys_mkdirat(int dirfd, const char* path, int mode);
extern int sys_utimensat(int dirfd, const char* path, const void* times, int flags);
extern int fs_mount_module_root(void);
extern int fs_get_mount_status(char* buf, size_t size);

void syscall_dispatch(struct syscall_frame* frame) {
    uint64_t syscall_no = frame->rax;
    struct task* current = get_current_task();
    (void)current;
    kernel_lock_enter();
#if ORTHOX_MEM_PROGRESS
    trace_progress_bump(current, &current->trace_syscalls);
    trace_progress_bump_syscall(current, syscall_no, frame->rdx);
#endif
    switch (syscall_no) {
        case SYS_READ:
            frame->rax = (uint64_t)sys_read((int)frame->rdi, (void*)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_WRITE:
            frame->rax = (uint64_t)sys_write((int)frame->rdi, (const void*)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_OPEN:
            frame->rax = (uint64_t)sys_open((const char*)frame->rdi, (int)frame->rsi, (int)frame->rdx);
            break;
        case SYS_OPENAT:
            frame->rax = (uint64_t)sys_openat((int)frame->rdi, (const char*)frame->rsi, (int)frame->rdx, (int)frame->r10);
            break;
        case SYS_CLOSE:
            frame->rax = (uint64_t)sys_close((int)frame->rdi);
            break;
        case SYS_STAT:
            frame->rax = (uint64_t)sys_stat((const char*)frame->rdi, (struct kstat*)frame->rsi);
            break;
        case SYS_FSTAT:
            frame->rax = (uint64_t)sys_fstat((int)frame->rdi, (struct kstat*)frame->rsi);
            break;
        case SYS_LSTAT:
            frame->rax = (uint64_t)sys_stat((const char*)frame->rdi, (struct kstat*)frame->rsi);
            break;
        case SYS_ACCESS:
            frame->rax = (uint64_t)sys_access((const char*)frame->rdi, (int)frame->rsi);
            break;
        case SYS_FSTATAT:
            frame->rax = (uint64_t)sys_fstatat((int)frame->rdi, (const char*)frame->rsi, (struct kstat*)frame->rdx, (int)frame->r10);
            break;
        case SYS_READLINK:
            frame->rax = (uint64_t)sys_readlink((const char*)frame->rdi, (char*)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_READLINKAT:
            frame->rax = (uint64_t)sys_readlinkat((int)frame->rdi, (const char*)frame->rsi, (char*)frame->rdx, (size_t)frame->r10);
            break;
        case SYS_FACCESSAT:
            frame->rax = (uint64_t)sys_faccessat((int)frame->rdi, (const char*)frame->rsi, (int)frame->rdx, (int)frame->r10);
            break;
        case SYS_UTIMENSAT:
            frame->rax = (uint64_t)sys_utimensat((int)frame->rdi, (const char*)frame->rsi,
                                                 (const void*)frame->rdx, (int)frame->r10);
            break;
        case SYS_LSEEK:
            frame->rax = (uint64_t)sys_lseek((int)frame->rdi, (int64_t)frame->rsi, (int)frame->rdx);
            break;
        case SYS_FTRUNCATE:
            frame->rax = (uint64_t)sys_ftruncate((int)frame->rdi, frame->rsi);
            break;
        case SYS_TRUNCATE:
            frame->rax = (uint64_t)sys_truncate((const char*)frame->rdi, frame->rsi);
            break;
        case SYS_MMAP:
#if ORTHOX_MEM_PROGRESS
            trace_progress_bump(current, &current->trace_mmap_calls);
#endif
            frame->rax = (uint64_t)sys_mmap((void*)frame->rdi, (size_t)frame->rsi, (int)frame->rdx, (int)frame->r10, (int)frame->r8, (int64_t)frame->r9);
            memtrace_mmap("mmap", frame->rdi, frame->rsi, frame->rax,
                          (int)frame->rdx, (int)frame->r10, (int)frame->r8,
                          align_up_page(frame->rsi) / PAGE_SIZE);
            break;
        case SYS_MREMAP:
#if ORTHOX_MEM_PROGRESS
            trace_progress_bump(current, &current->trace_mremap_calls);
#endif
            frame->rax = (uint64_t)sys_mremap((void*)frame->rdi, (size_t)frame->rsi, (size_t)frame->rdx,
                                              (int)frame->r10, (void*)frame->r8);
            memtrace_mmap("mremap", frame->rdi, frame->rdx, frame->rax,
                          0, (int)frame->r10, -1,
                          align_up_page(frame->rdx) / PAGE_SIZE);
            break;
        case SYS_MADVISE:
            frame->rax = (uint64_t)sys_madvise((void*)frame->rdi, (size_t)frame->rsi, (int)frame->rdx);
            break;
        case SYS_MPROTECT:
            frame->rax = (uint64_t)sys_mprotect((void*)frame->rdi, (size_t)frame->rsi, (int)frame->rdx);
            memtrace_mmap("mprotect", frame->rdi, frame->rsi, frame->rax,
                          (int)frame->rdx, 0, -1,
                          align_up_page(frame->rsi) / PAGE_SIZE);
            break;
        case SYS_MUNMAP:
#if ORTHOX_MEM_PROGRESS
            trace_progress_bump(current, &current->trace_munmap_calls);
#endif
            frame->rax = (uint64_t)sys_munmap((void*)frame->rdi, (size_t)frame->rsi);
            memtrace_mmap("munmap", frame->rdi, frame->rsi, frame->rax,
                          0, 0, -1,
                          align_up_page(frame->rsi) / PAGE_SIZE);
            break;
        case SYS_IOCTL:
            frame->rax = (uint64_t)sys_ioctl((int)frame->rdi, (unsigned long)frame->rsi, frame->rdx);
            break;
        case SYS_WRITEV:
            frame->rax = (uint64_t)sys_writev((int)frame->rdi, (const struct orth_iovec*)frame->rsi, (int)frame->rdx);
            break;
        case SYS_READV:
            frame->rax = (uint64_t)sys_readv((int)frame->rdi, (const struct orth_iovec*)frame->rsi, (int)frame->rdx);
            break;
        case SYS_PIPE:
            {
                int pipefd[2];
                int ret = sys_pipe(pipefd);
                if (ret == 0) {
                    int* user_pipefd = (int*)frame->rdi;
                    user_pipefd[0] = pipefd[0];
                    user_pipefd[1] = pipefd[1];
                }
                frame->rax = (uint64_t)ret;
            }
            break;
        case SYS_PIPE2:
            {
                int pipefd[2];
                int ret = sys_pipe2(pipefd, (int)frame->rsi);
                if (ret == 0) {
                    int* user_pipefd = (int*)frame->rdi;
                    user_pipefd[0] = pipefd[0];
                    user_pipefd[1] = pipefd[1];
                }
                frame->rax = (uint64_t)ret;
            }
            break;
        case SYS_DUP2:
            frame->rax = (uint64_t)sys_dup2((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_SCHED_YIELD:
            frame->rax = (uint64_t)sys_sched_yield();
            break;
        case SYS_UNLINK:
            frame->rax = (uint64_t)sys_unlink((const char*)frame->rdi);
            break;
        case SYS_UNLINKAT:
            frame->rax = (uint64_t)sys_unlinkat((int)frame->rdi, (const char*)frame->rsi, (int)frame->rdx);
            break;
        case SYS_RENAME:
            frame->rax = (uint64_t)sys_rename((const char*)frame->rdi, (const char*)frame->rsi);
            break;
        case SYS_CHMOD:
            frame->rax = (uint64_t)sys_chmod((const char*)frame->rdi, (uint32_t)frame->rsi);
            break;
        case SYS_EXIT:
            sys_exit((int)frame->rdi);
            break;
        case SYS_BRK:
#if ORTHOX_MEM_PROGRESS
            trace_progress_bump(current, &current->trace_brk_calls);
#endif
            frame->rax = sys_brk(frame->rdi);
            break;
        case SYS_RT_SIGACTION:
            frame->rax = (uint64_t)sys_rt_sigaction((int)frame->rdi,
                                                    (const struct linux_rt_sigaction_k*)frame->rsi,
                                                    (struct linux_rt_sigaction_k*)frame->rdx,
                                                    (size_t)frame->r10);
            break;
        case SYS_RT_SIGPROCMASK:
            frame->rax = (uint64_t)sys_rt_sigprocmask((int)frame->rdi,
                                                      (const uint64_t*)frame->rsi,
                                                      (uint64_t*)frame->rdx,
                                                      (size_t)frame->r10);
            break;
        case SYS_NANOSLEEP:
            frame->rax = (uint64_t)sys_nanosleep((const struct orth_timespec_k*)frame->rdi,
                                                 (struct orth_timespec_k*)frame->rsi);
            break;
        case SYS_GETPID:
            frame->rax = sys_getpid();
            break;
        case SYS_GETPPID:
            frame->rax = sys_getppid();
            break;
        case SYS_GETUID:
            frame->rax = sys_getuid();
            break;
        case SYS_GETGID:
            frame->rax = sys_getgid();
            break;
        case SYS_GETEUID:
            frame->rax = sys_geteuid();
            break;
        case SYS_GETEGID:
            frame->rax = sys_getegid();
            break;
        case SYS_SOCKET:
            frame->rax = (uint64_t)sys_socket((int)frame->rdi, (int)frame->rsi, (int)frame->rdx);
            break;
        case SYS_CONNECT:
            frame->rax = (uint64_t)sys_connect((int)frame->rdi, (const void*)frame->rsi, (uint32_t)frame->rdx);
            break;
        case SYS_ACCEPT:
            frame->rax = (uint64_t)sys_accept((int)frame->rdi, (void*)frame->rsi, (uint32_t*)frame->rdx);
            break;
        case SYS_SENDTO:
            frame->rax = (uint64_t)sys_sendto((int)frame->rdi, (const void*)frame->rsi, (size_t)frame->rdx,
                                              (int)frame->r10, (const void*)frame->r8, (uint32_t)frame->r9);
            break;
        case SYS_RECVFROM:
            frame->rax = (uint64_t)sys_recvfrom((int)frame->rdi, (void*)frame->rsi, (size_t)frame->rdx,
                                                (int)frame->r10, (void*)frame->r8, (uint32_t*)frame->r9);
            break;
        case SYS_SHUTDOWN:
            frame->rax = (uint64_t)sys_shutdown((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_BIND:
            frame->rax = (uint64_t)sys_bind((int)frame->rdi, (const void*)frame->rsi, (uint32_t)frame->rdx);
            break;
        case SYS_LISTEN:
            frame->rax = (uint64_t)sys_listen((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_GETSOCKNAME:
            frame->rax = (uint64_t)sys_getsockname((int)frame->rdi, (void*)frame->rsi, (uint32_t*)frame->rdx);
            break;
        case SYS_GETPEERNAME:
            frame->rax = (uint64_t)sys_getpeername((int)frame->rdi, (void*)frame->rsi, (uint32_t*)frame->rdx);
            break;
        case SYS_SETSOCKOPT:
            frame->rax = (uint64_t)sys_setsockopt((int)frame->rdi, (int)frame->rsi, (int)frame->rdx,
                                                  (const void*)frame->r10, (uint32_t)frame->r8);
            break;
        case SYS_FORK:
            frame->rax = (uint64_t)task_fork(frame);
            break;
        case SYS_EXECVE:
            frame->rax = (uint64_t)task_execve(frame, (const char*)frame->rdi, (char* const*)frame->rsi, (char* const*)frame->rdx);
            break;
        case SYS_WAIT4:
            frame->rax = (uint64_t)sys_wait4((int)frame->rdi, (int*)frame->rsi, (int)frame->rdx);
            break;
        case SYS_SYSINFO:
            frame->rax = (uint64_t)sys_sysinfo((struct linux_sysinfo_k*)frame->rdi);
            break;
        case SYS_GETCWD:
            frame->rax = (uint64_t)sys_getcwd((char*)frame->rdi, (size_t)frame->rsi);
            break;
        case SYS_CHDIR:
            frame->rax = (uint64_t)sys_chdir((const char*)frame->rdi);
            break;
        case SYS_FCHDIR:
            frame->rax = (uint64_t)sys_fchdir((int)frame->rdi);
            break;
        case SYS_MKDIR:
            frame->rax = (uint64_t)sys_mkdir((const char*)frame->rdi, (int)frame->rsi);
            break;
        case SYS_MKDIRAT:
            frame->rax = (uint64_t)sys_mkdirat((int)frame->rdi, (const char*)frame->rsi, (int)frame->rdx);
            break;
        case SYS_RMDIR:
            frame->rax = (uint64_t)sys_rmdir((const char*)frame->rdi);
            break;
        case SYS_ARCH_PRCTL:
            frame->rax = (uint64_t)sys_arch_prctl((int)frame->rdi, frame->rsi);
            break;
        case SYS_SYNC:
            frame->rax = (uint64_t)sys_sync();
            break;
        case SYS_FUTEX:
            frame->rax = (uint64_t)sys_futex((volatile int*)frame->rdi, (int)frame->rsi, (int)frame->rdx);
            break;
        case SYS_KILL:
            frame->rax = (uint64_t)sys_kill((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_UNAME:
            frame->rax = (uint64_t)sys_uname((struct linux_utsname_k*)frame->rdi);
            break;
        case SYS_GETRLIMIT:
            frame->rax = (uint64_t)sys_getrlimit((unsigned)frame->rdi, (struct linux_rlimit_k*)frame->rsi);
            break;
        case SYS_GETTIMEOFDAY:
            frame->rax = (uint64_t)sys_gettimeofday((struct orth_timeval_k*)frame->rdi);
            break;
        case SYS_GETPGRP:
            frame->rax = (uint64_t)sys_getpgrp();
            break;
        case SYS_CLOCK_GETTIME:
            frame->rax = (uint64_t)sys_clock_gettime((int)frame->rdi, (struct orth_timespec_k*)frame->rsi);
            break;
        case SYS_SETPGID:
            frame->rax = (uint64_t)sys_setpgid((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_SETSID:
            frame->rax = (uint64_t)sys_setsid();
            break;
        case ORTH_SYS_TCGETPGRP:
            frame->rax = (uint64_t)sys_tcgetpgrp((int)frame->rdi);
            break;
        case ORTH_SYS_TCSETPGRP:
            frame->rax = (uint64_t)sys_tcsetpgrp((int)frame->rdi, (int)frame->rsi);
            break;
        case ORTH_SYS_TCGETATTR:
            frame->rax = (uint64_t)sys_tcgetattr((int)frame->rdi, (struct orth_termios*)frame->rsi);
            break;
        case ORTH_SYS_TCSETATTR:
            frame->rax = (uint64_t)sys_tcsetattr((int)frame->rdi, (int)frame->rsi, (const struct orth_termios*)frame->rdx);
            break;
        case ORTH_SYS_SIGPROCMASK:
            frame->rax = (uint64_t)sys_sigprocmask((int)frame->rdi, (const uint64_t*)frame->rsi, (uint64_t*)frame->rdx);
            break;
        case ORTH_SYS_SIGPENDING:
            frame->rax = (uint64_t)sys_sigpending((uint64_t*)frame->rdi);
            break;
        case ORTH_SYS_SIGACTION:
            frame->rax = (uint64_t)sys_sigaction((int)frame->rdi, (const struct orth_sigaction*)frame->rsi, (struct orth_sigaction*)frame->rdx);
            break;
        case SYS_FCNTL:
            frame->rax = (uint64_t)sys_fcntl((int)frame->rdi, (int)frame->rsi, frame->rdx);
            break;
        case ORTH_SYS_LS:
            sys_ls();
            break;
        case ORTH_SYS_GET_VIDEO_INFO:
            frame->rax = (uint64_t)sys_get_video_info((struct video_info*)frame->rdi);
            break;
        case ORTH_SYS_MAP_FRAMEBUFFER:
            frame->rax = sys_map_framebuffer();
            break;
        case ORTH_SYS_GET_TICKS_MS:
            frame->rax = sys_get_ticks_ms();
            break;
        case ORTH_SYS_SLEEP_MS:
            frame->rax = (uint64_t)sys_sleep_ms(frame->rdi);
            break;
        case ORTH_SYS_GET_KEY_EVENT:
            frame->rax = (uint64_t)sys_get_key_event((struct key_event*)frame->rdi);
            break;
        case ORTH_SYS_SOUND_ON:
            frame->rax = (uint64_t)sys_sound_on((uint32_t)frame->rdi);
            break;
        case ORTH_SYS_SOUND_OFF:
            frame->rax = (uint64_t)sys_sound_off();
            break;
        case ORTH_SYS_SOUND_PCM_U8:
            frame->rax = (uint64_t)sys_sound_pcm_u8((const uint8_t*)frame->rdi, (uint32_t)frame->rsi, (uint32_t)frame->rdx);
            break;
        case ORTH_SYS_USB_INFO:
            usb_dump_status();
            frame->rax = (uint64_t)usb_is_ready();
            break;
        case ORTH_SYS_USB_READ_BLOCK:
            frame->rax = (uint64_t)sys_usb_read_block((uint32_t)frame->rdi, (void*)frame->rsi, (uint32_t)frame->rdx);
            break;
        case ORTH_SYS_MOUNT_USB_ROOT:
            frame->rax = (uint64_t)-1;
            break;
        case ORTH_SYS_MOUNT_MODULE_ROOT:
            frame->rax = (uint64_t)fs_mount_module_root();
            break;
        case ORTH_SYS_GET_MOUNT_STATUS:
            frame->rax = (uint64_t)fs_get_mount_status((char*)frame->rdi, (size_t)frame->rsi);
            break;
        case ORTH_SYS_DNS_LOOKUP:
            frame->rax = (uint64_t)lwip_port_lookup_ipv4((const char*)frame->rdi, (uint32_t*)frame->rsi);
            break;
        case ORTH_SYS_GET_CPU_ID:
            {
                struct cpu_local* cpu = get_cpu_local();
                frame->rax = (uint64_t)(cpu ? (int)cpu->cpu_id : -1);
            }
            break;
        case ORTH_SYS_SET_FORK_SPREAD:
            frame->rax = (uint64_t)sys_set_fork_spread((int)frame->rdi);
            break;
        case ORTH_SYS_GET_FORK_SPREAD:
            frame->rax = (uint64_t)sys_get_fork_spread();
            break;
        case ORTH_SYS_GET_RUNQ_STATS:
            frame->rax = (uint64_t)task_get_runq_stats((struct orth_runq_stat*)frame->rdi, (uint32_t)frame->rsi);
            break;
        case SYS_GETDENTS:
            frame->rax = (uint64_t)sys_getdents((int)frame->rdi, (struct orth_dirent*)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_GETDENTS64:
            frame->rax = (uint64_t)sys_getdents64((int)frame->rdi, (void*)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_SET_ROBUST_LIST:
            frame->rax = (uint64_t)sys_set_robust_list((const void*)frame->rdi, (size_t)frame->rsi);
            break;
        case SYS_SET_TID_ADDRESS:
            frame->rax = (uint64_t)sys_set_tid_address((int*)frame->rdi);
            break;
        case SYS_PRLIMIT64:
            frame->rax = (uint64_t)sys_prlimit64((int)frame->rdi, (unsigned)frame->rsi,
                                                 (const struct linux_rlimit_k*)frame->rdx,
                                                 (struct linux_rlimit_k*)frame->r10);
            break;
        case SYS_GETRANDOM:
            frame->rax = (uint64_t)sys_getrandom((void*)frame->rdi, (size_t)frame->rsi, (unsigned)frame->rdx);
            break;
        case SYS_SIGALTSTACK:
            frame->rax = (uint64_t)sys_sigaltstack((const struct linux_stack_t_k*)frame->rdi,
                                                   (struct linux_stack_t_k*)frame->rsi);
            break;
        case SYS_EXIT_GROUP:
            sys_exit((int)frame->rdi);
            break;
        default:
            frame->rax = (uint64_t)-38;
            break;
    }
    // Timer IRQ sets resched pending; perform context switch at syscall boundary,
    // not in interrupt return path, to avoid iretq frame corruption.
    if (task_consume_resched()) {
        kernel_yield();
    }
    kernel_lock_exit();
}
