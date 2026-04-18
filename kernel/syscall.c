#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
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

void puts(const char *s);
void puthex(uint64_t v);

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084
#define MSR_FS_BASE 0xC0000100

#define ORTH_TCGETS    0x5401
#define ORTH_TCSETS    0x5402
#define ORTH_TIOCGPGRP 0x540F
#define ORTH_TIOCSPGRP 0x5410
#define ORTH_TIOCGWINSZ 0x5413

extern void syscall_entry(void);
extern struct task* task_list;
extern volatile struct limine_memmap_request memmap_request;
extern int64_t sys_write(int fd, const void* buf, size_t count);
extern int64_t sys_read(int fd, void* buf, size_t count);
extern int sys_close(int fd);

void sys_brk_init(uint64_t initial_break) {
    (void)initial_break;
}

struct syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

static void* kernel_memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

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

static inline void outb_u8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb_u8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint64_t rdtsc_u64(void) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
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

struct orth_timeval_k {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct orth_timespec_k {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_rt_sigaction_k {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint32_t mask[2];
};

static int is_leap_year(int year) {
    if ((year % 4) != 0) return 0;
    if ((year % 100) != 0) return 1;
    return (year % 400) == 0;
}

static uint32_t days_before_month(int year, int month) {
    static const uint16_t base[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    uint32_t days = 0;
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    days = base[month - 1];
    if (month > 2 && is_leap_year(year)) days++;
    return days;
}

static uint64_t days_since_year_zero(int year, int month, int day) {
    uint64_t y = (uint64_t)year;
    uint64_t days = y * 365ULL + y / 4ULL - y / 100ULL + y / 400ULL;
    days += days_before_month(year, month);
    if (day > 0) days += (uint64_t)(day - 1);
    return days;
}

static uint8_t cmos_read(uint8_t reg) {
    outb_u8(0x70, reg);
    return inb_u8(0x71);
}

static int cmos_bcd_to_bin(int value) {
    return ((value >> 4) * 10) + (value & 0x0F);
}

static uint64_t sys_realtime_seconds(void) {
    int second;
    int minute;
    int hour;
    int day;
    int month;
    int year;
    int regb;
    uint64_t days;

    while (cmos_read(0x0A) & 0x80) {
    }

    second = cmos_read(0x00);
    minute = cmos_read(0x02);
    hour = cmos_read(0x04);
    day = cmos_read(0x07);
    month = cmos_read(0x08);
    year = cmos_read(0x09);
    regb = cmos_read(0x0B);

    if ((regb & 0x04) == 0) {
        second = cmos_bcd_to_bin(second);
        minute = cmos_bcd_to_bin(minute);
        hour = cmos_bcd_to_bin(hour & 0x7F) | (hour & 0x80);
        day = cmos_bcd_to_bin(day);
        month = cmos_bcd_to_bin(month);
        year = cmos_bcd_to_bin(year);
    }

    if ((regb & 0x02) == 0 && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    year += (year >= 70) ? 1900 : 2000;
    days = days_since_year_zero(year, month, day);
    if (days < 719528ULL) return 0;
    return (days - 719528ULL) * 86400ULL
        + (uint64_t)hour * 3600ULL
        + (uint64_t)minute * 60ULL
        + (uint64_t)second;
}

static int sys_gettimeofday(struct orth_timeval_k* tv) {
    if (!tv) return -1;
    tv->tv_sec = (int64_t)sys_realtime_seconds();
    tv->tv_usec = 0;
    return 0;
}

static int sys_clock_gettime(int clock_id, struct orth_timespec_k* ts) {
    uint64_t ms;
    if (!ts) return -1;
    if (clock_id == 0) {
        ts->tv_sec = (int64_t)sys_realtime_seconds();
        ts->tv_nsec = 0;
        return 0;
    }
    if (clock_id != 1) return -1;
    ms = lapic_get_ticks_ms();
    ts->tv_sec = (int64_t)(ms / 1000ULL);
    ts->tv_nsec = (int64_t)((ms % 1000ULL) * 1000000ULL);
    return 0;
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

static struct task* find_task_by_pid_locked(int pid) {
    if (!kernel_lock_held()) {
        puts("[warn] find_task_by_pid_locked without BKL\r\n");
        return NULL;
    }
    struct task* t = task_list;
    while (t) {
        if (t->pid == pid) return t;
        t = t->next;
    }
    return NULL;
}

static void task_signal_add_locked(struct task* t, int sig) {
    if (!kernel_lock_held()) {
        puts("[warn] task_signal_add_locked without BKL\r\n");
        return;
    }
    if (!t || sig <= 0 || sig >= 64) return;
    if (sig < 32 && t->sig_handlers[sig] == 1ULL) return;
    t->sig_pending |= (1ULL << sig);
    if (t->state == TASK_SLEEPING) {
        task_wake(t);
    }
}

static int task_signal_pending(const struct task* t, int sig) {
    if (!t || sig <= 0 || sig >= 64) return 0;
    return (t->sig_pending & (1ULL << sig)) != 0;
}

static void sys_exit(int status) {
    struct task* current = get_current_task();
    // Ensure no speaker tone leaks when a user task exits abruptly.
    sound_beep_stop();
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (current->fds[fd].in_use) {
            (void)sys_close(fd);
        }
    }
    task_mark_zombie(current, status);
    if (current->ppid > 0) {
        struct task* parent = find_task_by_pid_locked(current->ppid);
        if (parent) task_signal_add_locked(parent, 20);
    }
    while(1) kernel_yield();
}

static uint64_t sys_brk(uint64_t addr) {
    struct task* current = get_current_task();
    if (addr == 0 || addr <= current->heap_break) {
        return current->heap_break;
    }
    uint64_t current_page = (current->heap_break + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    uint64_t target_page = (addr + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(current->ctx.cr3);
    while (current_page < target_page) {
        void* phys_mem = pmm_alloc(1);
        if (!phys_mem) {
            puts("[sys_brk] pmm_alloc failed!\r\n");
            return current->heap_break;
        }
        kernel_memset(PHYS_TO_VIRT(phys_mem), 0, PAGE_SIZE);
        vmm_map_page(pml4, current_page, (uint64_t)phys_mem, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        current_page += PAGE_SIZE;
    }
    current->heap_break = addr;
    return current->heap_break;
}

static int64_t sys_wait4(int pid, int* wstatus, int options) {
    (void)options;
    struct task* current = get_current_task();
    while (1) {
        int found_child = 0;
        struct task* curr = task_list;
        while (curr) {
            if (curr->ppid == current->pid) {
                if (pid == -1 || curr->pid == pid) {
                    found_child = 1;
                    if (curr->state == TASK_ZOMBIE) {
                        int child_pid = curr->pid;
                        if (wstatus) *wstatus = curr->exit_status << 8;
                        current->sig_pending &= ~(1ULL << 20);
                        (void)task_reap(curr);
                        return child_pid;
                    }
                }
            }
            curr = curr->next;
        }
        if (!found_child) return -1;
        task_mark_sleeping(current);
        if (task_signal_pending(current, 20)) {
            if (current->state == TASK_SLEEPING) {
                task_wake(current);
            }
            continue;
        }
        kernel_yield();
    }
}

#define MMAP_BASE_ADDR 0x0000200000000000ULL
#define MMAP_TOP_ADDR  0x00007F0000000000ULL

static uint64_t align_up_page(uint64_t v) {
    return (v + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static int is_user_mmap_range_valid(uint64_t vaddr, uint64_t size) {
    if (size == 0) return 0;
    if ((vaddr & (PAGE_SIZE - 1)) != 0) return 0;
    if (vaddr < MMAP_BASE_ADDR) return 0;
    if (vaddr >= MMAP_TOP_ADDR) return 0;
    if (vaddr + size < vaddr) return 0;
    if (vaddr + size > MMAP_TOP_ADDR) return 0;
    return 1;
}

static int is_range_unmapped(uint64_t* pml4, uint64_t vaddr, uint64_t size) {
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        if (vmm_get_phys(pml4, vaddr + off) != 0) {
            return 0;
        }
    }
    return 1;
}

static void unmap_one_page(uint64_t* pml4, uint64_t vaddr) {
    if (!(pml4[PML4_IDX(vaddr)] & PTE_PRESENT)) return;
    uint64_t* pdp = (uint64_t*)PHYS_TO_VIRT(pml4[PML4_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pdp[PDP_IDX(vaddr)] & PTE_PRESENT)) return;
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdp[PDP_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pd[PD_IDX(vaddr)] & PTE_PRESENT)) return;
    if (pd[PD_IDX(vaddr)] & PTE_HUGE) return; // mmap() does not create huge pages here.
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(vaddr)] & PTE_ADDR_MASK);
    uint64_t* pte = &pt[PT_IDX(vaddr)];
    if (!(*pte & PTE_PRESENT)) return;

    void* page_phys = (void*)(*pte & PTE_ADDR_MASK);
    *pte = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    pmm_free(page_phys, 1);
}

static void rollback_mmap(uint64_t* pml4, uint64_t base, uint64_t mapped_size) {
    for (uint64_t off = 0; off < mapped_size; off += PAGE_SIZE) {
        unmap_one_page(pml4, base + off);
    }
}

static void copy_mmap_file_page(uint8_t* dest, file_descriptor_t* f, uint64_t file_off) {
    if (!f || !dest) return;
    size_t size = fs_fd_size(f);
    void* data = fs_fd_data(f);
    if (file_off >= size || !data) return;
    uint64_t remain = size - file_off;
    if (remain > PAGE_SIZE) remain = PAGE_SIZE;
    uint8_t* src = (uint8_t*)data + file_off;
    for (uint64_t i = 0; i < remain; i++) {
        dest[i] = src[i];
    }
}

static void* sys_mmap(void* addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    struct task* current = get_current_task();
    if (!current || length == 0) return (void*)-1;
    if (!(flags & (MAP_PRIVATE | MAP_SHARED))) return (void*)-1;
    if (offset < 0) return (void*)-1;
    if ((offset & (PAGE_SIZE - 1)) != 0) return (void*)-1;

    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(current->ctx.cr3);
    uint64_t size = align_up_page((uint64_t)length);
    if (size == 0) return (void*)-1;

    file_descriptor_t* backing_fd = 0;
    int is_anonymous = (flags & MAP_ANONYMOUS) != 0;
    if (is_anonymous) {
        if (fd != -1 && fd != 0) return (void*)-1;
        if (offset != 0) return (void*)-1;
    } else {
        if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return (void*)-1;
        backing_fd = &current->fds[fd];
        if (backing_fd->type == FT_PIPE) return (void*)-1;
    }

    uint64_t base = align_up_page((uint64_t)addr);
    if (base == 0 || !(flags & MAP_FIXED)) {
        if (current->mmap_end < MMAP_BASE_ADDR) {
            current->mmap_end = MMAP_BASE_ADDR;
        }
        base = align_up_page(current->mmap_end);
        while (base < MMAP_TOP_ADDR && !is_range_unmapped(pml4, base, size)) {
            base = align_up_page(base + PAGE_SIZE);
        }
        if (base >= MMAP_TOP_ADDR) return (void*)-1;
        current->mmap_end = align_up_page(base + size);
    } else {
        if ((uint64_t)addr != base) return (void*)-1;
        if (!is_user_mmap_range_valid(base, size)) return (void*)-1;
        if (!is_range_unmapped(pml4, base, size)) {
            // Linux MAP_FIXED replaces overlapping mapping; keep same behavior.
            for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
                unmap_one_page(pml4, base + off);
            }
        }
    }

    if (!is_user_mmap_range_valid(base, size)) return (void*)-1;

    uint64_t map_flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) {
        map_flags |= PTE_WRITABLE;
    }

    uint64_t mapped = 0;
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        void* phys = pmm_alloc(1);
        if (!phys) {
            rollback_mmap(pml4, base, mapped);
            return (void*)-1;
        }
        kernel_memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        if (!is_anonymous) {
            copy_mmap_file_page((uint8_t*)PHYS_TO_VIRT(phys), backing_fd, (uint64_t)offset + off);
        }
        vmm_map_page(pml4, base + off, (uint64_t)phys, map_flags);
        mapped += PAGE_SIZE;
    }

    return (void*)base;
}

static int sys_munmap(void* addr, size_t length) {
    struct task* current = get_current_task();
    if (!current || length == 0) return -1;

    uint64_t base = (uint64_t)addr;
    if (base & (PAGE_SIZE - 1)) return -1;
    uint64_t size = align_up_page((uint64_t)length);
    if (!is_user_mmap_range_valid(base, size)) return -1;

    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(current->ctx.cr3);
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        unmap_one_page(pml4, base + off);
    }
    return 0;
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

struct linux_sysinfo_k {
    unsigned long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
    char __reserved[256];
};

static int sys_sysinfo(struct linux_sysinfo_k* info) {
    struct limine_memmap_response* memmap;
    uint64_t totalram = 0;

    if (!info) return -1;

    memmap = memmap_request.response;
    if (memmap) {
        for (uint64_t i = 0; i < memmap->entry_count; i++) {
            struct limine_memmap_entry* entry = memmap->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE) {
                totalram += entry->length;
            }
        }
    }

    *info = (struct linux_sysinfo_k){0};
    info->uptime = lapic_get_ticks_ms() / 1000ULL;
    info->totalram = (unsigned long)totalram;
    info->freeram = (unsigned long)totalram;
    info->procs = 1;
    info->mem_unit = 1;
    return 0;
}

static int sys_sleep_ms(uint64_t ms) {
    struct task* current = get_current_task();
    if (!current) return -1;
    task_mark_sleeping(current);
    current->sleep_until_ms = lapic_get_ticks_ms() + ms;
    while (current->state == TASK_SLEEPING) {
        kernel_yield();
    }
    return 0;
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

static uint64_t sys_getpid(void) {
    return (uint64_t)get_current_task()->pid;
}

static uint64_t sys_getppid(void) {
    struct task* current = get_current_task();
    return (uint64_t)(current ? current->ppid : 0);
}

static uint64_t sys_getuid(void) {
    return 0;
}

static uint64_t sys_getgid(void) {
    return 0;
}

static uint64_t sys_geteuid(void) {
    return 0;
}

static uint64_t sys_getegid(void) {
    return 0;
}

static int g_tty_pgrp = 0;
static struct orth_termios g_console_termios = {
    .c_iflag = 0x00000002u,
    .c_oflag = 0x00000001u,
    .c_cflag = 0,
    .c_lflag = 0x00000001u | 0x00000002u | 0x00000008u,
    .c_cc = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26 },
    .c_ispeed = 115200,
    .c_ospeed = 115200,
};

static int sys_kill(int pid, int sig) {
    struct task* current = get_current_task();
    struct task* t = NULL;
    if (sig == 0) {
        if (pid > 0) return find_task_by_pid_locked(pid) ? 0 : -1;
        return 0;
    }
    if (pid > 0) {
        t = find_task_by_pid_locked(pid);
    } else if (pid == 0 && current) {
        t = current;
    }
    if (!t) return -1;
    task_signal_add_locked(t, sig);
    if (sig == 2 || sig == 15 || sig == 9) {
        task_mark_zombie(t, 128 + sig);
        if (t->ppid > 0) {
            struct task* parent = find_task_by_pid_locked(t->ppid);
            if (parent) task_signal_add_locked(parent, 20);
        }
        return 0;
    }
    return 0;
}

static int sys_getpgrp(void) {
    struct task* current = get_current_task();
    return current ? current->pgid : -1;
}

static int sys_arch_prctl(int code, uint64_t addr) {
    struct task* current = get_current_task();
    if (!current) return -1;

    switch (code) {
        case ARCH_SET_FS:
            current->user_fs_base = addr;
            wrmsr(MSR_FS_BASE, addr);
            return 0;
        case ARCH_GET_FS:
            *(uint64_t*)addr = current->user_fs_base;
            return 0;
        default:
            return -1;
    }
}

static int sys_futex(volatile int* uaddr, int op, int val) {
    int cmd = op & ~FUTEX_PRIVATE;
    if (!uaddr) return -1;

    switch (cmd) {
        case FUTEX_WAIT:
            /* Single-threaded for now: only validate the expected value. */
            return (*uaddr == val) ? 0 : -11;
        case FUTEX_WAKE:
            return 0;
        default:
            return -1;
    }
}

static int sys_setpgid(int pid, int pgid) {
    struct task* current = get_current_task();
    struct task* target = NULL;
    if (!current) return -1;
    if (pid == 0) pid = current->pid;
    if (pgid == 0) pgid = pid;
    target = find_task_by_pid_locked(pid);
    if (!target) return -1;
    target->pgid = pgid;
    return 0;
}

static int sys_setsid(void) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (current->pgid == current->pid) return -1;
    current->sid = current->pid;
    current->pgid = current->pid;
    g_tty_pgrp = current->pgid;
    return current->sid;
}

static int sys_tcgetpgrp(int fd) {
    (void)fd;
    if (g_tty_pgrp == 0) {
        struct task* current = get_current_task();
        if (current) g_tty_pgrp = current->pgid;
    }
    return g_tty_pgrp;
}

static int sys_tcsetpgrp(int fd, int pgrp) {
    (void)fd;
    g_tty_pgrp = pgrp;
    return 0;
}

int tty_get_foreground_pgrp(void) {
    if (g_tty_pgrp == 0) {
        struct task* current = get_current_task();
        if (current) g_tty_pgrp = current->pgid;
    }
    return g_tty_pgrp;
}

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

static int sys_set_tid_address(int* tidptr) {
    struct task* current = get_current_task();
    (void)tidptr;
    return current ? current->pid : -1;
}

static int sys_sigprocmask(int how, const uint64_t* set, uint64_t* oldset) {
    struct task* current = get_current_task();
    uint64_t newmask;
    if (!current) return -1;
    if (oldset) *oldset = current->sig_mask;
    if (!set) return 0;
    newmask = *set;
    switch (how) {
        case 0:
            current->sig_mask = newmask;
            break;
        case 1:
            current->sig_mask |= newmask;
            break;
        case 2:
            current->sig_mask &= ~newmask;
            break;
        default:
            return -1;
    }
    return 0;
}

static int sys_rt_sigprocmask(int how, const uint64_t* set, uint64_t* oldset, size_t sigsetsize) {
    if (sigsetsize < sizeof(uint64_t)) return -1;
    return sys_sigprocmask(how, set, oldset);
}

static int sys_sigpending(uint64_t* set) {
    struct task* current = get_current_task();
    if (!current || !set) return -1;
    *set = current->sig_pending & current->sig_mask;
    return 0;
}

static int sys_sigaction(int sig, const struct orth_sigaction* act, struct orth_sigaction* oldact) {
    struct task* current = get_current_task();
    if (!current || sig <= 0 || sig >= 32) return -1;
    if (oldact) {
        oldact->sa_handler = current->sig_handlers[sig];
        oldact->sa_mask = current->sig_action_masks[sig];
        oldact->sa_flags = current->sig_action_flags[sig];
        oldact->reserved = 0;
    }
    if (act) {
        current->sig_handlers[sig] = act->sa_handler;
        current->sig_action_masks[sig] = act->sa_mask;
        current->sig_action_flags[sig] = act->sa_flags;
        if (act->sa_handler == 1ULL) {
            current->sig_pending &= ~(1ULL << sig);
        }
    }
    return 0;
}

static int sys_rt_sigaction(int sig, const struct linux_rt_sigaction_k* act,
                            struct linux_rt_sigaction_k* oldact, size_t sigsetsize) {
    struct orth_sigaction in_act;
    struct orth_sigaction out_act;
    int ret;

    if (sigsetsize < sizeof(uint64_t)) return -1;
    if (act) {
        in_act.sa_handler = act->handler;
        in_act.sa_mask = ((uint64_t)act->mask[1] << 32) | act->mask[0];
        in_act.sa_flags = (uint32_t)act->flags;
        in_act.reserved = 0;
    }
    ret = sys_sigaction(sig, act ? &in_act : 0, oldact ? &out_act : 0);
    if (ret < 0) return ret;
    if (oldact) {
        oldact->handler = out_act.sa_handler;
        oldact->flags = out_act.sa_flags;
        oldact->restorer = 0;
        oldact->mask[0] = (uint32_t)(out_act.sa_mask & 0xffffffffU);
        oldact->mask[1] = (uint32_t)(out_act.sa_mask >> 32);
    }
    return 0;
}

extern int task_fork(struct syscall_frame* frame);
extern int task_execve(struct syscall_frame* frame, const char* path, char* const argv[], char* const envp[]);
extern int64_t sys_write(int fd, const void* buf, size_t count);
extern int64_t sys_read(int fd, void* buf, size_t count);
extern int sys_open(const char* path, int flags, int mode);
extern int sys_openat(int dirfd, const char* path, int flags, int mode);
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
    kernel_lock_enter();
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
        case SYS_MMAP:
            frame->rax = (uint64_t)sys_mmap((void*)frame->rdi, (size_t)frame->rsi, (int)frame->rdx, (int)frame->r10, (int)frame->r8, (int64_t)frame->r9);
            break;
        case SYS_MUNMAP:
            frame->rax = (uint64_t)sys_munmap((void*)frame->rdi, (size_t)frame->rsi);
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
        case SYS_SET_TID_ADDRESS:
            frame->rax = (uint64_t)sys_set_tid_address((int*)frame->rdi);
            break;
        case SYS_GETRANDOM:
            frame->rax = (uint64_t)sys_getrandom((void*)frame->rdi, (size_t)frame->rsi, (unsigned)frame->rdx);
            break;
        case SYS_EXIT_GROUP:
            sys_exit((int)frame->rdi);
            break;
        default:
            frame->rax = (uint64_t)-1;
            break;
    }
    // Timer IRQ sets resched pending; perform context switch at syscall boundary,
    // not in interrupt return path, to avoid iretq frame corruption.
    if (task_consume_resched()) {
        kernel_yield();
    }
    kernel_lock_exit();
}
