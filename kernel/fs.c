#include "fs.h"
#include "task.h"
#include "limine.h"
#include "pmm.h"
#include "vmm.h"
#include "usb.h"
#include "lapic.h"
#include "net_socket.h"
#include "spinlock.h"

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 14
#define FD_CLOEXEC 1
#define ORTH_O_ACCMODE 3
#define ORTH_AT_FDCWD (-100)
#define ORTH_AT_REMOVEDIR 0x200
#define ORTH_LINUX_O_CLOEXEC 0x80000
#define ORTH_LINUX_O_NONBLOCK 0x800

extern volatile struct limine_module_request module_request;
extern void puts(const char* s);
extern void puthex(uint64_t v);

#define ORTH_LINUX_O_CREAT   0x40
#define ORTH_LINUX_O_TRUNC   0x200
#define ORTH_LINUX_O_APPEND  0x400
#define ORTH_LEGACY_O_DIRECTORY 0x200000

static void pipe_wake_task(struct task* waiter) {
    if (!waiter) return;
    if (waiter->state == TASK_SLEEPING) {
        task_wake(waiter);
    }
}

static struct task* pipe_take_waiter_locked(struct task** waiter) {
    struct task* task;
    if (!waiter) return 0;
    task = *waiter;
    *waiter = 0;
    return task;
}

static void pipe_set_waiter(struct task** waiter, struct task* task) {
    if (!waiter) return;
    *waiter = task;
}

static void pipe_clear_waiter(struct task** waiter, struct task* task) {
    if (!waiter) return;
    if (*waiter == task) *waiter = 0;
}

static int strcmp_exact(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        if (*s1 != *s2) return 0;
        s1++; s2++;
    }
    return *s1 == *s2;
}

static int strcmp_suffix_fs(const char* full_path, const char* suffix) {
    int len_f = 0; while(full_path[len_f]) len_f++;
    int len_s = 0; while(suffix[len_s]) len_s++;
    if (len_f < len_s) return 0;
    for (int i = 0; i < len_s; i++) {
        if (full_path[len_f - len_s + i] != suffix[i]) return 0;
    }
    return 1;
}

static const char* normalize_fs_path(const char* path) {
    while (*path == '/') path++;
    if (path[0] == '.' && path[1] == '/') {
        path += 2;
        while (*path == '/') path++;
    }
    return path;
}

static void canonicalize_path_inplace(char* path) {
    char tmp[256];
    size_t ti = 0;
    size_t i = 0;

    if (!path) return;
    if (path[0] != '/') {
        tmp[ti++] = '/';
    }

    while (path[i] && ti + 1 < sizeof(tmp)) {
        while (path[i] == '/') i++;
        if (!path[i]) break;

        if (path[i] == '.' && (path[i + 1] == '/' || path[i + 1] == '\0')) {
            i += (path[i + 1] == '/') ? 2 : 1;
            continue;
        }
        if (path[i] == '.' && path[i + 1] == '.' && (path[i + 2] == '/' || path[i + 2] == '\0')) {
            i += (path[i + 2] == '/') ? 3 : 2;
            if (ti > 1) {
                ti--;
                while (ti > 1 && tmp[ti - 1] != '/') ti--;
            }
            continue;
        }

        if (ti == 0 || tmp[ti - 1] != '/') tmp[ti++] = '/';
        while (path[i] && path[i] != '/' && ti + 1 < sizeof(tmp)) {
            tmp[ti++] = path[i++];
        }
    }

    if (ti == 0) tmp[ti++] = '/';
    tmp[ti] = '\0';
    i = 0;
    while (tmp[i]) {
        path[i] = tmp[i];
        i++;
    }
    path[i] = '\0';
}

static int path_like_equal(const char* a, const char* b) {
    int ai = 0;
    int bi = 0;
    while (a[ai] && b[bi]) {
        if (a[ai] != b[bi]) return 0;
        ai++;
        bi++;
    }
    while (a[ai] == '/') ai++;
    while (b[bi] == '/') bi++;
    return a[ai] == '\0' && b[bi] == '\0';
}

static int tar_name_matches(const char* tar_name, const char* path) {
    return path_like_equal(normalize_fs_path(tar_name), normalize_fs_path(path));
}

static int starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int contains_char(const char* s, char c) {
    if (!s) return 0;
    while (*s) {
        if (*s == c) return 1;
        s++;
    }
    return 0;
}

static void resolve_task_path(const char* path, char* out, size_t size) {
    struct task* current = get_current_task();
    size_t i = 0;
    size_t j = 0;
    if (!out || size == 0) return;
    if (!path || path[0] == '\0') {
        out[0] = '\0';
        return;
    }
    if (path[0] == '/' || !current || current->cwd[0] == '\0') {
        while (path[j] && i + 1 < size) out[i++] = path[j++];
        out[i] = '\0';
        canonicalize_path_inplace(out);
        return;
    }
    if (current->cwd[0] == '/') {
        out[i++] = '/';
        j = 1;
    }
    while (current->cwd[j] && i + 1 < size) out[i++] = current->cwd[j++];
    if (i > 0 && out[i - 1] != '/' && i + 1 < size) out[i++] = '/';
    j = 0;
    while (path[j] && i + 1 < size) out[i++] = path[j++];
    out[i] = '\0';
    canonicalize_path_inplace(out);
}

static int resolve_dirfd_path(int dirfd, const char* path, char* out, size_t size) {
    struct task* current = get_current_task();
    const char* base = 0;
    size_t i = 0;
    size_t j = 0;
    if (!out || size == 0 || !path) return -1;
    if (path[0] == '/') {
        resolve_task_path(path, out, size);
        return 0;
    }
    if (dirfd == ORTH_AT_FDCWD) {
        resolve_task_path(path, out, size);
        return 0;
    }
    if (!current || dirfd < 0 || dirfd >= MAX_FDS || !current->fds[dirfd].in_use) return -1;
    if (current->fds[dirfd].type != FT_DIR || current->fds[dirfd].name[0] == '\0') return -1;
    base = current->fds[dirfd].name;
    while (base[j] && i + 1 < size) out[i++] = base[j++];
    if (i > 0 && out[i - 1] != '/' && i + 1 < size) out[i++] = '/';
    j = 0;
    while (path[j] && i + 1 < size) out[i++] = path[j++];
    out[i] = '\0';
    canonicalize_path_inplace(out);
    return 0;
}

// 8進数文字列を数値に変換 (TARヘッダ用)
static uint64_t oct2int(const char* s, int size) {
    uint64_t res = 0;
    for (int i = 0; i < size; i++) {
        if (s[i] < '0' || s[i] > '7') break;
        res = res * 8 + (s[i] - '0');
    }
    return res;
}

struct fat_boot_info {
    uint32_t part_start;
    uint32_t part_sectors;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fats;
    uint32_t fat_size;
    uint32_t root_cluster;
    uint32_t data_start_lba;
};

struct fat_dir_entry_info {
    char name[256];
    uint8_t attr;
    uint32_t first_cluster;
    uint32_t size;
};

struct fat_lfn_state {
    char name[256];
    int valid;
};

#define USB_READ_RETRIES 3

static int usb_read_blocks_safe(uint32_t lba, void* buf, uint32_t count) {
    uint8_t* dst = (uint8_t*)buf;
    for (int attempt = 0; attempt < USB_READ_RETRIES; attempt++) {
        if (usb_read_block(lba, dst, count) == 0) return 0;
    }
    for (uint32_t done = 0; done < count; done++) {
        for (int attempt = 0; attempt < USB_READ_RETRIES; attempt++) {
            if (usb_read_block(lba + done, dst + done * 512U, 1) == 0) break;
            if (attempt == USB_READ_RETRIES - 1) return -1;
        }
    }
    return 0;
}

static void fat_copy_entry(struct fat_dir_entry_info* dst, const struct fat_dir_entry_info* src) {
    if (!dst || !src) return;
    for (int i = 0; i < (int)sizeof(dst->name); i++) {
        dst->name[i] = src->name[i];
        if (src->name[i] == '\0') break;
    }
    dst->name[sizeof(dst->name) - 1] = '\0';
    dst->attr = src->attr;
    dst->first_cluster = src->first_cluster;
    dst->size = src->size;
}

static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void format_83_name(const uint8_t* e, char* name) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) name[n++] = (char)e[i];
    if (e[8] != ' ') {
        name[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) name[n++] = (char)e[i];
    }
    name[n] = '\0';
}

static void fat_lfn_reset(struct fat_lfn_state* st) {
    if (!st) return;
    st->name[0] = '\0';
    st->valid = 0;
}

static void fat_lfn_append_chunk(struct fat_lfn_state* st, const uint8_t* e) {
    static const uint8_t offsets[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    int order;
    int base;
    if (!st || !e) return;
    order = (e[0] & 0x1F);
    if (order <= 0) return;
    if (e[0] & 0x40) {
        fat_lfn_reset(st);
        st->valid = 1;
    }
    if (!st->valid) return;
    base = (order - 1) * 13;
    if (base < 0 || base >= (int)sizeof(st->name)) {
        fat_lfn_reset(st);
        return;
    }
    for (int i = 0; i < 13 && (base + i) < (int)sizeof(st->name) - 1; i++) {
        uint16_t ch = read_le16(&e[offsets[i]]);
        if (ch == 0x0000 || ch == 0xFFFF) {
            st->name[base + i] = '\0';
            return;
        }
        st->name[base + i] = (ch < 0x80) ? (char)ch : '?';
    }
}

static void fat_decode_entry(const uint8_t* e, struct fat_lfn_state* lfn, struct fat_dir_entry_info* ent) {
    ent->attr = e[11];
    format_83_name(e, ent->name);
    if (lfn && lfn->valid && lfn->name[0] != '\0') {
        for (int i = 0; i < (int)sizeof(ent->name) - 1; i++) {
            ent->name[i] = lfn->name[i];
            if (lfn->name[i] == '\0') break;
        }
        ent->name[sizeof(ent->name) - 1] = '\0';
    }
    ent->first_cluster = ((uint32_t)read_le16(&e[20]) << 16) | read_le16(&e[26]);
    ent->size = read_le32(&e[28]);
    if (lfn) fat_lfn_reset(lfn);
}

static int usb_get_first_partition(uint32_t* start, uint32_t* sectors) {
    uint8_t sector[512];
    for (int attempt = 0; attempt < USB_READ_RETRIES; attempt++) {
        if (usb_read_blocks_safe(0, sector, 1) == 0) break;
        if (attempt == USB_READ_RETRIES - 1) return -1;
    }
    *start = read_le32(&sector[446 + 8]);
    *sectors = read_le32(&sector[446 + 12]);
    return (*start && *sectors) ? 0 : -1;
}

static int usb_load_fat_boot(struct fat_boot_info* info) {
    uint8_t sector[512];
    if (!info) return -1;
    if (usb_get_first_partition(&info->part_start, &info->part_sectors) < 0) return -1;
    for (int attempt = 0; attempt < USB_READ_RETRIES; attempt++) {
        if (usb_read_blocks_safe(info->part_start, sector, 1) == 0) break;
        if (attempt == USB_READ_RETRIES - 1) return -1;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) return -1;
    info->bytes_per_sector = read_le16(&sector[11]);
    info->sectors_per_cluster = sector[13];
    info->reserved_sectors = read_le16(&sector[14]);
    info->fats = sector[16];
    info->fat_size = read_le32(&sector[36]);
    info->root_cluster = read_le32(&sector[44]);
    if (info->bytes_per_sector != 512 || info->sectors_per_cluster == 0) return -1;
    info->data_start_lba = info->part_start + info->reserved_sectors + (uint32_t)info->fats * info->fat_size;
    return 0;
}

static uint32_t fat_cluster_to_lba(const struct fat_boot_info* info, uint32_t cluster) {
    return info->data_start_lba + (cluster - 2U) * (uint32_t)info->sectors_per_cluster;
}

static uint32_t fat_read_next_cluster(const struct fat_boot_info* info, uint32_t cluster) {
    static uint32_t cached_lba = 0xFFFFFFFFU;
    static uint8_t cached_sector[512];
    static int cache_valid = 0;
    uint8_t sector[512];
    uint32_t fat_offset = cluster * 4U;
    uint32_t fat_lba = info->part_start + info->reserved_sectors + (fat_offset / 512U);
    uint32_t ent_off = fat_offset % 512U;
    if (!cache_valid || cached_lba != fat_lba) {
        if (usb_read_blocks_safe(fat_lba, sector, 1) < 0) return 0x0FFFFFFFU;
        for (int i = 0; i < 512; i++) cached_sector[i] = sector[i];
        cached_lba = fat_lba;
        cache_valid = 1;
    }
    return read_le32(&cached_sector[ent_off]) & 0x0FFFFFFFU;
}

static int fat_read_dir_entry(const struct fat_boot_info* info, uint32_t dir_cluster,
                              const char* target, struct fat_dir_entry_info* out) {
    uint8_t sector[4096];
    uint32_t cluster = dir_cluster;
    struct fat_lfn_state lfn;
    if (!info || !target || !out || cluster < 2) return -1;
    fat_lfn_reset(&lfn);
    while (cluster >= 2 && cluster < 0x0FFFFFF8U) {
        uint32_t lba = fat_cluster_to_lba(info, cluster);
        uint32_t total = (uint32_t)info->bytes_per_sector * info->sectors_per_cluster;
        if (total > sizeof(sector)) return -1;
        if (usb_read_blocks_safe(lba, sector, info->sectors_per_cluster) < 0) return -1;
        for (uint32_t off = 0; off < total; off += 32) {
            uint8_t* e = &sector[off];
            if (e[0] == 0x00) return -1;
            if (e[0] == 0xE5) {
                fat_lfn_reset(&lfn);
                continue;
            }
            if (e[11] == 0x0F) {
                fat_lfn_append_chunk(&lfn, e);
                continue;
            }
            fat_decode_entry(e, &lfn, out);
            if (!strcmp_exact(out->name, target)) continue;
            return 0;
        }
        cluster = fat_read_next_cluster(info, cluster);
    }
    return -1;
}

static int fat_resolve_path(const char* path, struct fat_dir_entry_info* out) {
    struct fat_boot_info info;
    char path_buf[128];
    char* p;
    uint32_t dir_cluster;
    struct fat_dir_entry_info ent;
    if (!path || !out) return -1;
    while (*path == '/') path++;
    if (!starts_with(path, "usb/")) return -1;
    path += 4;
    if (*path == '\0') return -1;
    if (usb_load_fat_boot(&info) < 0) return -1;
    for (int i = 0; i < 127; i++) {
        path_buf[i] = path[i];
        if (path[i] == '\0') break;
    }
    path_buf[127] = '\0';
    dir_cluster = info.root_cluster;
    p = path_buf;
    while (*p == '/') p++;
    while (*p) {
        char* part = p;
        while (*p && *p != '/') p++;
        if (*p) {
            *p = '\0';
            p++;
            while (*p == '/') p++;
        }
        if (fat_read_dir_entry(&info, dir_cluster, part, &ent) < 0) return -1;
        dir_cluster = ent.first_cluster;
        fat_copy_entry(out, &ent);
    }
    return 0;
}

#define MAX_RAMFS_FILES 16
#define RAMFS_FILE_SIZE 65536 // 簡易的に1ファイル64KB固定

struct ramfs_file {
    char name[64];
    uint8_t* data;
    size_t size;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t atime_sec;
    uint64_t mtime_sec;
    uint64_t ctime_sec;
    int in_use;
};

static struct ramfs_file ramfs_table[MAX_RAMFS_FILES];

enum mount_type {
    MOUNT_NONE = 0,
    MOUNT_USB_FAT = 1,
};

enum root_source_type {
    ROOT_SOURCE_MODULE = 0,
    ROOT_SOURCE_USB = 1,
};

struct mount_point {
    const char* path;
    enum mount_type type;
    int active;
};

static struct mount_point g_mounts[] = {
    { "usb", MOUNT_USB_FAT, 1 },
};

#define DIR_BUF_PAGES 4
#define DIR_BUF_BYTES (DIR_BUF_PAGES * PAGE_SIZE)

#define USB_ROOT_CACHE_SLOTS 4

struct usb_root_cache_entry {
    int valid;
    uint32_t mode;
    size_t size;
    uint64_t pages;
    void* data;
    char path[128];
};

static enum root_source_type g_root_source = ROOT_SOURCE_MODULE;
static uint8_t* g_usb_root_tar = 0;
static size_t g_usb_root_tar_size = 0;
static uint64_t g_usb_root_tar_pages = 0;
static char g_usb_root_tar_path[128];
static struct fat_dir_entry_info g_usb_root_tar_ent;
static int g_usb_root_tar_file_ready = 0;
static struct usb_root_cache_entry g_usb_root_cache[USB_ROOT_CACHE_SLOTS];
static uint32_t g_usb_root_cache_next = 0;

static uint64_t fs_now_sec(void) {
    return lapic_get_ticks_ms() / 1000U;
}

enum {
    FS_DEV_CONSOLE = 1,
    FS_DEV_PIPE = 2,
    FS_DEV_RAMFS = 3,
    FS_DEV_ROOT_ARCHIVE = 4,
    FS_DEV_USB_ROOT_ARCHIVE = 5,
    FS_DEV_MODULE = 6,
    FS_DEV_USB_FAT = 7,
    FS_DEV_SYNTH = 8,
};

static uint64_t fs_hash_name(const char* s) {
    uint64_t h = 1469598103934665603ULL;
    if (!s) return 0;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

static uint32_t fs_default_mode_for_type(uint32_t type_bits) {
    if ((type_bits & 0170000U) == KSTAT_MODE_DIR) return KSTAT_MODE_DIR | 0555U;
    if ((type_bits & 0170000U) == KSTAT_MODE_CHR) return KSTAT_MODE_CHR | 0666U;
    return KSTAT_MODE_FILE | 0444U;
}

static void kstat_set_defaults(struct kstat* st, uint32_t mode, int64_t size) {
    if (!st) return;
    st->dev = 0;
    st->ino = 0;
    st->mode = mode;
    st->uid = 0;
    st->gid = 0;
    st->nlink = ((mode & 0170000U) == KSTAT_MODE_DIR) ? 2U : 1U;
    st->rdev = 0;
    st->size = size;
    st->atime_sec = 0;
    st->mtime_sec = 0;
    st->ctime_sec = 0;
}

static void kstat_from_ramfs(struct kstat* st, const struct ramfs_file* rf) {
    if (!st || !rf) return;
    st->dev = FS_DEV_RAMFS;
    st->ino = fs_hash_name(rf->name);
    st->mode = rf->mode;
    st->uid = rf->uid;
    st->gid = rf->gid;
    st->nlink = rf->nlink;
    st->rdev = 0;
    st->size = (int64_t)rf->size;
    st->atime_sec = (int64_t)rf->atime_sec;
    st->mtime_sec = (int64_t)rf->mtime_sec;
    st->ctime_sec = (int64_t)rf->ctime_sec;
}

static uint32_t tar_mode_from_header(const struct tar_header* h) {
    uint32_t type_bits = (h && h->typeflag == '5') ? KSTAT_MODE_DIR : KSTAT_MODE_FILE;
    uint32_t perms = h ? (uint32_t)(oct2int(h->mode, 8) & 07777U) : 0;
    return type_bits | perms;
}

static void kstat_from_tar_header(struct kstat* st, const struct tar_header* h, uint64_t size) {
    int64_t mtime = h ? (int64_t)oct2int(h->mtime, 12) : 0;
    if (!st) return;
    st->dev = FS_DEV_ROOT_ARCHIVE;
    st->ino = h ? fs_hash_name(h->name) : 0;
    st->mode = tar_mode_from_header(h);
    st->uid = h ? (uint32_t)oct2int(h->uid, 8) : 0;
    st->gid = h ? (uint32_t)oct2int(h->gid, 8) : 0;
    st->nlink = (h && h->typeflag == '5') ? 2U : 1U;
    st->rdev = 0;
    st->size = (int64_t)size;
    st->atime_sec = mtime;
    st->mtime_sec = mtime;
    st->ctime_sec = mtime;
}

static int usb_read_file_range(const struct fat_dir_entry_info* ent, uint64_t offset, void* out_buf, size_t size);
static int fs_try_active_root_lookup(const char* path, void** data, size_t* size, uint32_t* mode);
static int fs_try_active_root_stat(const char* path, struct kstat* st);

static int path_component_match(const char* path, const char* prefix) {
    while (*prefix) {
        if (*path != *prefix) return 0;
        path++;
        prefix++;
    }
    return (*path == '\0' || *path == '/');
}

static struct mount_point* fs_resolve_mount(const char* path, const char** subpath) {
    struct mount_point* best = 0;
    size_t best_len = 0;
    const char* norm = normalize_fs_path(path);
    for (size_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
        size_t len = 0;
        if (!g_mounts[i].active) continue;
        while (g_mounts[i].path[len]) len++;
        if (!path_component_match(norm, g_mounts[i].path)) continue;
        if (len < best_len) continue;
        best = &g_mounts[i];
        best_len = len;
    }
    if (!best) return 0;
    if (subpath) {
        *subpath = norm + best_len;
        if (**subpath == '/') (*subpath)++;
    }
    return best;
}

static void usb_root_cache_reset(void) {
    for (int i = 0; i < USB_ROOT_CACHE_SLOTS; i++) {
        if (g_usb_root_cache[i].valid && g_usb_root_cache[i].data) {
            pmm_free((void*)VIRT_TO_PHYS((uint64_t)g_usb_root_cache[i].data), (int)g_usb_root_cache[i].pages);
        }
        g_usb_root_cache[i].valid = 0;
        g_usb_root_cache[i].mode = 0;
        g_usb_root_cache[i].size = 0;
        g_usb_root_cache[i].pages = 0;
        g_usb_root_cache[i].data = 0;
        g_usb_root_cache[i].path[0] = '\0';
    }
    g_usb_root_cache_next = 0;
}

static struct usb_root_cache_entry* usb_root_cache_find(const char* path) {
    const char* norm = normalize_fs_path(path);
    for (int i = 0; i < USB_ROOT_CACHE_SLOTS; i++) {
        if (g_usb_root_cache[i].valid && strcmp_exact(g_usb_root_cache[i].path, norm)) {
            puts("[usbfs] cache hit ");
            puts(norm);
            puts("\r\n");
            return &g_usb_root_cache[i];
        }
    }
    return 0;
}

static void usb_root_cache_store(const char* path, uint32_t mode, void* data, size_t size, uint64_t pages) {
    struct usb_root_cache_entry* slot = &g_usb_root_cache[g_usb_root_cache_next % USB_ROOT_CACHE_SLOTS];
    const char* norm = normalize_fs_path(path);
    if (slot->valid && slot->data) {
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)slot->data), (int)slot->pages);
    }
    slot->valid = 1;
    slot->mode = mode;
    slot->size = size;
    slot->pages = pages;
    slot->data = data;
    for (int i = 0; i < (int)sizeof(slot->path) - 1; i++) {
        slot->path[i] = norm[i];
        if (norm[i] == '\0') break;
    }
    slot->path[sizeof(slot->path) - 1] = '\0';
    puts("[usbfs] cache store ");
    puts(slot->path);
    puts(" size=0x");
    puthex(size);
    puts("\r\n");
    g_usb_root_cache_next++;
}

static int fs_is_mount_dir(const char* path) {
    const char* subpath = 0;
    return fs_resolve_mount(path, &subpath) && subpath && *subpath == '\0';
}

static int dirent_name_exists(const struct orth_dirent* dirents, size_t count, const char* name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp_exact(dirents[i].name, name)) return 1;
    }
    return 0;
}

static int dirent_append(struct orth_dirent* dirents, size_t max_entries, size_t* count,
                         const char* name, uint32_t mode, uint32_t size) {
    size_t idx;
    if (!dirents || !count || !name || name[0] == '\0') return -1;
    if (dirent_name_exists(dirents, *count, name)) return 0;
    if (*count >= max_entries) return -1;
    idx = *count;
    dirents[idx].mode = mode;
    dirents[idx].size = size;
    for (int i = 0; i < (int)sizeof(dirents[idx].name) - 1; i++) {
        dirents[idx].name[i] = name[i];
        if (name[i] == '\0') break;
    }
    dirents[idx].name[sizeof(dirents[idx].name) - 1] = '\0';
    (*count)++;
    return 0;
}

static int build_ramfs_subdir_dirents(const char* path, struct orth_dirent* dirents,
                                      size_t max_entries, size_t* out_count) {
    const char* norm = normalize_fs_path(path);
    size_t count = out_count ? *out_count : 0;
    size_t prefix_len = 0;
    int found_dir = 0;
    int found_child = 0;

    while (norm[prefix_len]) prefix_len++;

    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        struct ramfs_file* rf;
        const char* rest;
        char child[248];
        int child_len = 0;
        uint32_t mode;

        if (!ramfs_table[i].in_use) continue;
        rf = &ramfs_table[i];
        if (!strcmp_exact(rf->name, norm) && ((rf->mode & 0170000U) == KSTAT_MODE_DIR)) {
            found_dir = 1;
            continue;
        }
        if (prefix_len == 0) continue;
        for (size_t j = 0; j < prefix_len; j++) {
            if (rf->name[j] != norm[j]) goto next_entry;
        }
        if (rf->name[prefix_len] != '/') goto next_entry;
        rest = rf->name + prefix_len + 1;
        if (*rest == '\0') goto next_entry;

        while (*rest && *rest != '/' && child_len < (int)sizeof(child) - 1) {
            child[child_len++] = *rest++;
        }
        child[child_len] = '\0';
        if (child[0] == '\0') goto next_entry;
        mode = (*rest == '/' || ((rf->mode & 0170000U) == KSTAT_MODE_DIR)) ? KSTAT_MODE_DIR : rf->mode;
        if (dirent_append(dirents, max_entries, &count, child, mode, (uint32_t)rf->size) < 0) return -1;
        found_child = 1;
next_entry:
        ;
    }

    *out_count = count;
    return (found_dir || found_child) ? 0 : -1;
}

static void tar_split_child(const char* tar_name, const char* dir_path,
                            char* child, uint32_t* mode, uint32_t file_size, char typeflag) {
    const char* norm;
    const char* rest;
    int i = 0;
    if (!child || !mode) return;
    child[0] = '\0';
    norm = normalize_fs_path(tar_name);
    rest = norm;
    if (dir_path && dir_path[0] != '\0') {
        const char* dir = normalize_fs_path(dir_path);
        while (*dir && *rest && *dir == *rest) {
            dir++;
            rest++;
        }
        if (*dir != '\0') return;
        if (*rest != '\0' && *rest != '/') return;
        if (*rest == '/') rest++;
        if (*rest == '\0') return;
    }
    while (*rest && *rest != '/' && i < 247) child[i++] = *rest++;
    child[i] = '\0';
    if (child[0] == '\0') return;
    *mode = (*rest == '/' || typeflag == '5') ? KSTAT_MODE_DIR : KSTAT_MODE_FILE;
    (void)file_size;
}

static int build_tar_dirents_from_archive(uint8_t* archive, size_t archive_size, const char* dir_path,
                                          struct orth_dirent* dirents, size_t max_entries, size_t* out_count) {
    uint8_t* ptr = archive;
    uint8_t* end = archive + archive_size;
    size_t count = out_count ? *out_count : 0;
    while (ptr < end) {
        struct tar_header* h = (struct tar_header*)ptr;
        uint64_t file_size;
        uint32_t mode = KSTAT_MODE_FILE;
        char child[248];
        if (h->name[0] == '\0') break;
        file_size = oct2int(h->size, 12);
        tar_split_child(h->name, dir_path, child, &mode, (uint32_t)file_size, h->typeflag);
        if (child[0] != '\0' && dirent_append(dirents, max_entries, &count, child, mode, (uint32_t)file_size) < 0) return -1;
        ptr += 512 + ((file_size + 511) & ~511);
    }
    *out_count = count;
    return 0;
}

static int build_tar_dirents_from_usb_root(const char* dir_path,
                                           struct orth_dirent* dirents, size_t max_entries, size_t* out_count) {
    uint64_t offset = 0;
    uint8_t header[512];
    size_t count = out_count ? *out_count : 0;
    while (offset + 512U <= g_usb_root_tar_ent.size) {
        struct tar_header* h = (struct tar_header*)header;
        uint64_t file_size;
        uint64_t padded_size;
        uint32_t mode = KSTAT_MODE_FILE;
        char child[248];
        if (usb_read_file_range(&g_usb_root_tar_ent, offset, header, sizeof(header)) < 0) return -1;
        if (h->name[0] == '\0') break;
        file_size = oct2int(h->size, 12);
        if (h->typeflag == '0' || h->typeflag == '\0') {
            padded_size = (file_size + 511U) & ~511U;
        } else {
            padded_size = 0;
        }
        tar_split_child(h->name, dir_path, child, &mode, (uint32_t)file_size, h->typeflag);
        if (child[0] != '\0' && dirent_append(dirents, max_entries, &count, child, mode, (uint32_t)file_size) < 0) return -1;
        offset += 512U + padded_size;
    }
    *out_count = count;
    return 0;
}

static int build_root_dirents(struct orth_dirent* dirents, size_t max_entries, size_t* out_count) {
    size_t count = 0;
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (!ramfs_table[i].in_use) continue;
        if (contains_char(ramfs_table[i].name, '/')) continue;
        if (dirent_append(dirents, max_entries, &count, ramfs_table[i].name, ramfs_table[i].mode, (uint32_t)ramfs_table[i].size) < 0) return -1;
    }
    if (g_root_source == ROOT_SOURCE_USB && g_usb_root_tar && g_usb_root_tar_size > 0) {
        if (build_tar_dirents_from_archive(g_usb_root_tar, g_usb_root_tar_size, "", dirents, max_entries, &count) < 0) return -1;
    } else if (g_root_source == ROOT_SOURCE_USB && g_usb_root_tar_file_ready) {
        if (build_tar_dirents_from_usb_root("", dirents, max_entries, &count) < 0) return -1;
    } else if (module_request.response) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file* m = module_request.response->modules[i];
            if (strcmp_suffix_fs(m->path, ".tar")) {
                if (build_tar_dirents_from_archive((uint8_t*)m->address, m->size, "", dirents, max_entries, &count) < 0) return -1;
                break;
            }
        }
    }
    if (usb_block_device_ready()) {
        for (size_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
            if (!g_mounts[i].active) continue;
            if (dirent_append(dirents, max_entries, &count, g_mounts[i].path, KSTAT_MODE_DIR, 0) < 0) return -1;
        }
    }
    *out_count = count;
    return 0;
}

static int build_usb_dirents(const char* path, struct orth_dirent* dirents, size_t max_entries, size_t* out_count) {
    struct fat_boot_info info;
    struct fat_dir_entry_info ent;
    uint32_t dir_cluster;
    uint8_t sector[4096];
    uint32_t cluster;
    struct fat_lfn_state lfn;
    size_t count = 0;
    const char* usb_path = 0;
    struct mount_point* mp = fs_resolve_mount(path, &usb_path);
    if (!mp || mp->type != MOUNT_USB_FAT) return -1;
    if (usb_load_fat_boot(&info) < 0) return -1;
    if (*usb_path == '\0') {
        dir_cluster = info.root_cluster;
    } else {
        char usb_full_path[320];
        int pos = 0;
        for (int i = 0; mp->path[i] && pos < (int)sizeof(usb_full_path) - 1; i++) usb_full_path[pos++] = mp->path[i];
        if (pos < (int)sizeof(usb_full_path) - 1) usb_full_path[pos++] = '/';
        for (int i = 0; usb_path[i] && pos < (int)sizeof(usb_full_path) - 1; i++) usb_full_path[pos++] = usb_path[i];
        usb_full_path[pos] = '\0';
        if (fat_resolve_path(usb_full_path, &ent) < 0) return -1;
        if ((ent.attr & 0x10U) == 0) return -1;
        dir_cluster = ent.first_cluster;
    }
    fat_lfn_reset(&lfn);
    cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8U) {
        uint32_t lba = fat_cluster_to_lba(&info, cluster);
        uint32_t total = (uint32_t)info.bytes_per_sector * info.sectors_per_cluster;
        if (total > sizeof(sector)) return -1;
        if (usb_read_blocks_safe(lba, sector, info.sectors_per_cluster) < 0) return -1;
        for (uint32_t off = 0; off < total; off += 32) {
            uint8_t* e = &sector[off];
            struct fat_dir_entry_info item;
            uint32_t mode;
            if (e[0] == 0x00) {
                *out_count = count;
                return 0;
            }
            if (e[0] == 0xE5) {
                fat_lfn_reset(&lfn);
                continue;
            }
            if (e[11] == 0x0F) {
                fat_lfn_append_chunk(&lfn, e);
                continue;
            }
            fat_decode_entry(e, &lfn, &item);
            if (strcmp_exact(item.name, ".") || strcmp_exact(item.name, "..")) continue;
            mode = (item.attr & 0x10U) ? KSTAT_MODE_DIR : KSTAT_MODE_FILE;
            if (dirent_append(dirents, max_entries, &count, item.name, mode, item.size) < 0) return -1;
        }
        cluster = fat_read_next_cluster(&info, cluster);
    }
    *out_count = count;
    return 0;
}

static struct ramfs_file* find_ramfs(const char* name);

static int build_active_root_dirents(const char* path, struct orth_dirent* dirents, size_t max_entries, size_t* out_count) {
    const char* norm = normalize_fs_path(path);
    size_t dir_size = 0;
    uint32_t dir_mode = 0;
    int found = 0;
    if (norm[0] == '\0') return build_root_dirents(dirents, max_entries, out_count);
    if (build_ramfs_subdir_dirents(norm, dirents, max_entries, out_count) == 0) {
        found = 1;
    }
    if (fs_try_active_root_lookup(norm, 0, &dir_size, &dir_mode) == 0) {
        if ((dir_mode & 0170000U) != KSTAT_MODE_DIR) return -1;
        found = 1;
        if (g_root_source == ROOT_SOURCE_USB && g_usb_root_tar && g_usb_root_tar_size > 0) {
            return build_tar_dirents_from_archive(g_usb_root_tar, g_usb_root_tar_size, norm, dirents, max_entries, out_count);
        }
        if (g_root_source == ROOT_SOURCE_USB && g_usb_root_tar_file_ready) {
            return build_tar_dirents_from_usb_root(norm, dirents, max_entries, out_count);
        }
        if (module_request.response) {
            for (uint64_t i = 0; i < module_request.response->module_count; i++) {
                struct limine_file* m = module_request.response->modules[i];
                if (!strcmp_suffix_fs(m->path, ".tar")) continue;
                return build_tar_dirents_from_archive((uint8_t*)m->address, m->size, norm, dirents, max_entries, out_count);
            }
        }
    }
    return found ? 0 : -1;
}

void fs_init(void) {
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        ramfs_table[i].in_use = 0;
        ramfs_table[i].data = NULL;
        ramfs_table[i].mode = 0;
        ramfs_table[i].uid = 0;
        ramfs_table[i].gid = 0;
        ramfs_table[i].nlink = 0;
        ramfs_table[i].atime_sec = 0;
        ramfs_table[i].mtime_sec = 0;
        ramfs_table[i].ctime_sec = 0;
    }
}

static struct ramfs_file* find_ramfs(const char* name) {
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (ramfs_table[i].in_use && strcmp_exact(ramfs_table[i].name, name)) {
            return &ramfs_table[i];
        }
    }
    return NULL;
}

static struct ramfs_file* create_ramfs(const char* name, uint32_t mode) {
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (!ramfs_table[i].in_use) {
            ramfs_table[i].in_use = 1;
            for (int j = 0; j < 63; j++) {
                ramfs_table[i].name[j] = name[j];
                if (name[j] == '\0') break;
            }
            ramfs_table[i].name[63] = '\0';
            void* phys = pmm_alloc(16);
            ramfs_table[i].data = (uint8_t*)PHYS_TO_VIRT(phys);
            for (int j = 0; j < RAMFS_FILE_SIZE; j++) ramfs_table[i].data[j] = 0;
            ramfs_table[i].size = 0;
            ramfs_table[i].mode = KSTAT_MODE_FILE | (mode & 07777U);
            ramfs_table[i].uid = 0;
            ramfs_table[i].gid = 0;
            ramfs_table[i].nlink = 1;
            ramfs_table[i].atime_sec = fs_now_sec();
            ramfs_table[i].mtime_sec = ramfs_table[i].atime_sec;
            ramfs_table[i].ctime_sec = ramfs_table[i].atime_sec;
            return &ramfs_table[i];
        }
    }
    return NULL;
}

static struct ramfs_file* create_ramfs_dir(const char* name, uint32_t mode) {
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (!ramfs_table[i].in_use) {
            ramfs_table[i].in_use = 1;
            for (int j = 0; j < 63; j++) {
                ramfs_table[i].name[j] = name[j];
                if (name[j] == '\0') break;
            }
            ramfs_table[i].name[63] = '\0';
            ramfs_table[i].data = NULL;
            ramfs_table[i].size = 0;
            ramfs_table[i].mode = KSTAT_MODE_DIR | (mode & 07777U);
            ramfs_table[i].uid = 0;
            ramfs_table[i].gid = 0;
            ramfs_table[i].nlink = 2;
            ramfs_table[i].atime_sec = fs_now_sec();
            ramfs_table[i].mtime_sec = ramfs_table[i].atime_sec;
            ramfs_table[i].ctime_sec = ramfs_table[i].atime_sec;
            return &ramfs_table[i];
        }
    }
    return NULL;
}

static int fs_try_archive_lookup(uint8_t* archive, size_t archive_size, const char* path, void** data, size_t* size, uint32_t* mode) {
    uint8_t* ptr = archive;
    uint8_t* end = archive + archive_size;
    while (ptr < end) {
        struct tar_header* h = (struct tar_header*)ptr;
        uint64_t file_size;
        if (h->name[0] == '\0') break;
        file_size = oct2int(h->size, 12);
        if (tar_name_matches(h->name, path)) {
            if (data) *data = ptr + 512;
            if (size) *size = file_size;
            if (mode) *mode = tar_mode_from_header(h);
            return 0;
        }
        ptr += 512 + ((file_size + 511) & ~511);
    }
    return -1;
}

static int fs_try_archive_stat_lookup(uint8_t* archive, size_t archive_size, const char* path, struct kstat* st) {
    uint8_t* ptr = archive;
    uint8_t* end = archive + archive_size;
    while (ptr < end) {
        struct tar_header* h = (struct tar_header*)ptr;
        uint64_t file_size;
        if (h->name[0] == '\0') break;
        file_size = oct2int(h->size, 12);
        if (tar_name_matches(h->name, path)) {
            kstat_from_tar_header(st, h, file_size);
            return 0;
        }
        ptr += 512 + ((file_size + 511) & ~511);
    }
    return -1;
}

static int usb_read_file_range(const struct fat_dir_entry_info* ent, uint64_t offset, void* out_buf, size_t size) {
    struct fat_boot_info info;
    uint32_t cluster;
    uint32_t cluster_size;
    uint64_t skip;
    size_t done = 0;
    uint8_t sector[4096];

    static uint32_t cache_first_cluster = 0;
    static uint64_t cache_base_offset = 0;
    static uint32_t cache_current_cluster = 0;

    if (!ent || !out_buf) return -1;
    if (offset + size > ent->size) return -1;
    if (usb_load_fat_boot(&info) < 0) return -1;

    cluster_size = (uint32_t)info.bytes_per_sector * info.sectors_per_cluster;
    if (cluster_size > sizeof(sector)) return -1;

    if (cache_first_cluster == ent->first_cluster && offset >= cache_base_offset) {
        cluster = cache_current_cluster;
        skip = offset - cache_base_offset;
    } else {
        cluster = ent->first_cluster;
        skip = offset;
    }

    while (skip >= cluster_size && cluster >= 2 && cluster < 0x0FFFFFF8U) {
        cluster = fat_read_next_cluster(&info, cluster);
        skip -= cluster_size;
    }

    cache_first_cluster = ent->first_cluster;
    cache_base_offset = offset & ~((uint64_t)cluster_size - 1);
    cache_current_cluster = cluster;

    while (done < size && cluster >= 2 && cluster < 0x0FFFFFF8U) {
        uint32_t lba = fat_cluster_to_lba(&info, cluster);
        uint32_t sector_index = (uint32_t)(skip / info.bytes_per_sector);
        uint32_t sector_off = (uint32_t)(skip % info.bytes_per_sector);
        uint32_t sectors_left = info.sectors_per_cluster - sector_index;
        size_t chunk = cluster_size - (size_t)skip;
        uint32_t sectors_needed;
        size_t max_chunk;
        if (chunk > size - done) chunk = size - done;
        sectors_needed = (uint32_t)((sector_off + chunk + info.bytes_per_sector - 1U) / info.bytes_per_sector);
        if (sectors_needed > sectors_left) sectors_needed = sectors_left;
        if (sectors_needed > 2U) sectors_needed = 2U;
        max_chunk = (size_t)sectors_needed * info.bytes_per_sector - sector_off;
        if (chunk > max_chunk) chunk = max_chunk;
        if (usb_read_blocks_safe(lba + sector_index, sector, sectors_needed) < 0) {
            return -1;
        }
        for (size_t i = 0; i < chunk; i++) ((uint8_t*)out_buf)[done + i] = sector[sector_off + i];
        done += chunk;
        skip += chunk;
        if (done >= size) break;
        if (skip >= cluster_size) {
            skip = 0;
            cluster = fat_read_next_cluster(&info, cluster);
            cache_base_offset += cluster_size;
            cache_current_cluster = cluster;
        }
    }
    return (done == size) ? 0 : -1;
}

static int fs_try_usb_root_tar_lookup(const char* path, void** data, size_t* size, uint32_t* mode) {
    uint64_t offset = 0;
    uint8_t header[512];
    struct usb_root_cache_entry* cache;
    if (!g_usb_root_tar_file_ready) return -1;
    cache = usb_root_cache_find(path);
    if (cache) {
        if (mode) *mode = cache->mode;
        if (size) *size = cache->size;
        if (data) *data = cache->data;
        return 0;
    }
    while (offset + 512U <= g_usb_root_tar_ent.size) {
        struct tar_header* h = (struct tar_header*)header;
        uint64_t file_size;
        uint64_t padded_size;
        if (usb_read_file_range(&g_usb_root_tar_ent, offset, header, sizeof(header)) < 0) return -1;
        if (h->name[0] == '\0') break;
        file_size = oct2int(h->size, 12);
        if (h->typeflag == '0' || h->typeflag == '\0') {
            padded_size = (file_size + 511U) & ~511U;
        } else {
            padded_size = 0;
        }
        if (tar_name_matches(h->name, path)) {
            if (mode) *mode = (h->typeflag == '5') ? KSTAT_MODE_DIR : KSTAT_MODE_FILE;
            if (size) *size = file_size;
            if (data) {
                uint64_t pages = (file_size + PAGE_SIZE - 1U) / PAGE_SIZE;
                uint8_t* buf;
                if (h->typeflag == '5' || file_size == 0) {
                    *data = 0;
                    return 0;
                }
                if (pages == 0) pages = 1;
                buf = (uint8_t*)PHYS_TO_VIRT(pmm_alloc((int)pages));
                if (!buf) return -1;
                if (usb_read_file_range(&g_usb_root_tar_ent, offset + 512U, buf, (size_t)file_size) < 0) {
                    pmm_free((void*)VIRT_TO_PHYS((uint64_t)buf), (int)pages);
                    return -1;
                }
                usb_root_cache_store(path, (h->typeflag == '5') ? KSTAT_MODE_DIR : KSTAT_MODE_FILE,
                                     buf, (size_t)file_size, pages);
                *data = buf;
            }
            return 0;
        }
        offset += 512U + padded_size;
    }
    return -1;
}

static int fs_try_usb_root_tar_stat_lookup(const char* path, struct kstat* st) {
    uint64_t offset = 0;
    uint8_t header[512];
    if (!g_usb_root_tar_file_ready) return -1;
    while (offset + 512U <= g_usb_root_tar_ent.size) {
        struct tar_header* h = (struct tar_header*)header;
        uint64_t file_size;
        uint64_t padded_size;
        if (usb_read_file_range(&g_usb_root_tar_ent, offset, header, sizeof(header)) < 0) return -1;
        if (h->name[0] == '\0') break;
        file_size = oct2int(h->size, 12);
        if (h->typeflag == '0' || h->typeflag == '\0') {
            padded_size = (file_size + 511U) & ~511U;
        } else {
            padded_size = 0;
        }
        if (tar_name_matches(h->name, path)) {
            kstat_from_tar_header(st, h, file_size);
            st->dev = FS_DEV_USB_ROOT_ARCHIVE;
            return 0;
        }
        offset += 512U + padded_size;
    }
    return -1;
}

static int fs_usb_root_tar_header_valid(void) {
    uint8_t header[512];
    struct tar_header* h = (struct tar_header*)header;
    if (!g_usb_root_tar_file_ready || g_usb_root_tar_ent.size < 512U) return -1;
    if (usb_read_file_range(&g_usb_root_tar_ent, 0, header, sizeof(header)) < 0) return -1;
    if (h->name[0] == '\0') return -1;
    if (h->magic[0] != 'u' || h->magic[1] != 's' || h->magic[2] != 't' ||
        h->magic[3] != 'a' || h->magic[4] != 'r') {
        return -1;
    }
    return 0;
}

static void fs_prefetch_usb_root_shell(void) {
    void* data = 0;
    size_t size = 0;
    uint32_t mode = 0;
    if (usb_root_cache_find("bin/sh")) return;
    if (fs_try_usb_root_tar_lookup("bin/sh", &data, &size, &mode) == 0) {
        puts("[usbfs] prefetched bin/sh\r\n");
    }
}

static int fs_try_active_root_lookup(const char* path, void** data, size_t* size, uint32_t* mode) {
    if (g_root_source == ROOT_SOURCE_USB && g_usb_root_tar && g_usb_root_tar_size > 0) {
        return fs_try_archive_lookup(g_usb_root_tar, g_usb_root_tar_size, path, data, size, mode);
    }
    if (g_root_source == ROOT_SOURCE_USB && g_usb_root_tar_file_ready) {
        return fs_try_usb_root_tar_lookup(path, data, size, mode);
    }
    if (module_request.response) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file* m = module_request.response->modules[i];
            if (strcmp_suffix_fs(m->path, ".tar") &&
                fs_try_archive_lookup((uint8_t*)m->address, m->size, path, data, size, mode) == 0) {
                return 0;
            }
        }
    }
    return -1;
}

static int fs_try_active_root_stat(const char* path, struct kstat* st) {
    if (g_root_source == ROOT_SOURCE_USB && g_usb_root_tar && g_usb_root_tar_size > 0) {
        if (fs_try_archive_stat_lookup(g_usb_root_tar, g_usb_root_tar_size, path, st) == 0) {
            st->dev = FS_DEV_USB_ROOT_ARCHIVE;
            return 0;
        }
        return -1;
    }
    if (g_root_source == ROOT_SOURCE_USB && g_usb_root_tar_file_ready) {
        return fs_try_usb_root_tar_stat_lookup(path, st);
    }
    if (module_request.response) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file* m = module_request.response->modules[i];
            if (strcmp_suffix_fs(m->path, ".tar") &&
                fs_try_archive_stat_lookup((uint8_t*)m->address, m->size, path, st) == 0) {
                return 0;
            }
        }
    }
    return -1;
}

static int fs_root_is_usb_only(void) {
    return g_root_source == ROOT_SOURCE_USB &&
           ((g_usb_root_tar && g_usb_root_tar_size > 0) || g_usb_root_tar_file_ready);
}

static void fs_list_archive_entries(uint8_t* archive, size_t archive_size, const char* label) {
    uint8_t* ptr = archive;
    uint8_t* end = archive + archive_size;
    if (!archive || archive_size == 0) return;
    puts(label);
    while (ptr < end) {
        struct tar_header* h = (struct tar_header*)ptr;
        uint64_t file_size;
        if (h->name[0] == '\0') break;
        file_size = oct2int(h->size, 12);
        puts(h->name);
        puts(" (TAR, size: 0x"); puthex(file_size); puts(")\n");
        ptr += 512 + ((file_size + 511) & ~511);
    }
}

static void fs_list_usb_root_tar_entries(const char* label) {
    uint64_t offset = 0;
    uint8_t header[512];
    if (!g_usb_root_tar_file_ready) return;
    puts(label);
    while (offset + 512U <= g_usb_root_tar_ent.size) {
        struct tar_header* h = (struct tar_header*)header;
        uint64_t file_size;
        uint64_t padded_size;
        if (usb_read_file_range(&g_usb_root_tar_ent, offset, header, sizeof(header)) < 0) {
            puts("(usb root listing failed)\n");
            return;
        }
        if (h->name[0] == '\0') break;
        file_size = oct2int(h->size, 12);
        if (h->typeflag == '0' || h->typeflag == '\0') {
            padded_size = (file_size + 511U) & ~511U;
        } else {
            padded_size = 0;
        }
        puts(h->name);
        puts(" (TAR, size: 0x"); puthex(file_size); puts(")\n");
        offset += 512U + padded_size;
    }
}

int fs_mount_usb_root_tar(const char* path) {
    const char* normalized = normalize_fs_path(path ? path : "usb/rootfs.tar");
    struct fat_dir_entry_info ent;
    if (!starts_with(normalized, "usb/")) return -1;
    if (fat_resolve_path(normalized, &ent) < 0) return -1;
    if (ent.attr & 0x10U) return -1;
    if (ent.size < 512) return -1;
    if (g_root_source == ROOT_SOURCE_USB && g_usb_root_tar) {
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)g_usb_root_tar), (int)g_usb_root_tar_pages);
    }
    g_usb_root_tar = 0;
    g_usb_root_tar_size = 0;
    g_usb_root_tar_pages = 0;
    usb_root_cache_reset();
    fat_copy_entry(&g_usb_root_tar_ent, &ent);
    g_usb_root_tar_file_ready = 1;
    if (fs_usb_root_tar_header_valid() < 0) {
        g_usb_root_tar_file_ready = 0;
        return -1;
    }
    fs_prefetch_usb_root_shell();
    g_root_source = ROOT_SOURCE_USB;
    for (int i = 0; i < (int)sizeof(g_usb_root_tar_path) - 1; i++) {
        g_usb_root_tar_path[i] = normalized[i];
        if (normalized[i] == '\0') break;
    }
    g_usb_root_tar_path[sizeof(g_usb_root_tar_path) - 1] = '\0';
    return 0;
}

int fs_mount_module_root(void) {
    g_root_source = ROOT_SOURCE_MODULE;
    g_usb_root_tar_file_ready = 0;
    usb_root_cache_reset();
    return 0;
}

int fs_get_mount_status(char* buf, size_t size) {
    const char* module_root = "root=module:boot/rootfs.tar\n/usb -> usb-fat";
    const char* usb_prefix = "root=usb:/";
    size_t i = 0;
    size_t j = 0;
    if (!buf || size == 0) return -1;
    if (g_root_source == ROOT_SOURCE_MODULE) {
        while (module_root[i] && i + 1 < size) {
            buf[i] = module_root[i];
            i++;
        }
    } else {
        while (usb_prefix[j] && i + 1 < size) buf[i++] = usb_prefix[j++];
        for (j = 0; g_usb_root_tar_path[j] && i + 1 < size; j++) {
            buf[i++] = g_usb_root_tar_path[j];
        }
        if (i + 1 < size) buf[i++] = '\n';
        {
            const char* mp = "/usb -> usb-fat";
            for (j = 0; mp[j] && i + 1 < size; j++) buf[i++] = mp[j];
        }
    }
    buf[(i < size) ? i : (size - 1)] = '\0';
    return 0;
}

int sys_open(const char* path, int flags, int mode) {
    struct task* current = get_current_task();
    int want_dir = (flags & (O_DIRECTORY | ORTH_LEGACY_O_DIRECTORY)) != 0;
    int want_creat = (flags & (O_CREAT | ORTH_LINUX_O_CREAT)) != 0;
    int want_trunc = (flags & (O_TRUNC | ORTH_LINUX_O_TRUNC)) != 0;
    const char* mount_subpath = 0;
    struct mount_point* mount = 0;
    char resolved_path[256];
    if (!current) return -1;

    int fd = -1;
    for (int i = 3; i < MAX_FDS; i++) {
        if (!current->fds[i].in_use) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return -1;

    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    path = normalize_fs_path(resolved_path);
    mount = fs_resolve_mount(path, &mount_subpath);

    if (want_dir) {
        struct orth_dirent* dirents;
        size_t entry_count = 0;
        void* phys = pmm_alloc(DIR_BUF_PAGES);
        if (!phys) return -1;
        dirents = (struct orth_dirent*)PHYS_TO_VIRT(phys);
        if (*path == '\0') {
            if (build_root_dirents(dirents, DIR_BUF_BYTES / sizeof(struct orth_dirent), &entry_count) < 0) {
                pmm_free(phys, DIR_BUF_PAGES);
                return -1;
            }
        } else if (mount && mount->type == MOUNT_USB_FAT) {
            if (build_usb_dirents(path, dirents, DIR_BUF_BYTES / sizeof(struct orth_dirent), &entry_count) < 0) {
                pmm_free(phys, DIR_BUF_PAGES);
                return -1;
            }
        } else {
            if (build_active_root_dirents(path, dirents, DIR_BUF_BYTES / sizeof(struct orth_dirent), &entry_count) < 0) {
                pmm_free(phys, DIR_BUF_PAGES);
                return -1;
            }
        }
        current->fds[fd].type = FT_DIR;
        current->fds[fd].data = dirents;
        current->fds[fd].size = entry_count * sizeof(struct orth_dirent);
        current->fds[fd].offset = 0;
        current->fds[fd].in_use = 1;
        current->fds[fd].flags = flags;
        current->fds[fd].aux0 = DIR_BUF_PAGES;
        current->fds[fd].aux1 = 0;
        if (*path == '\0') {
            current->fds[fd].name[0] = '/';
            current->fds[fd].name[1] = '\0';
        } else {
            for (int j = 0; j < 63; j++) {
                current->fds[fd].name[j] = resolved_path[j];
                if (resolved_path[j] == '\0') break;
            }
            current->fds[fd].name[63] = '\0';
        }
        return fd;
    }

    if (mount && mount->type == MOUNT_USB_FAT) {
        struct fat_dir_entry_info ent;
        if ((flags & O_WRONLY) || (flags & O_RDWR) || want_creat) return -1;
        if (fat_resolve_path(path, &ent) < 0) return -1;
        if (ent.attr & 0x10U) return -1;
        current->fds[fd].type = FT_USB;
        current->fds[fd].data = NULL;
        current->fds[fd].size = ent.size;
        current->fds[fd].offset = 0;
        current->fds[fd].in_use = 1;
        current->fds[fd].flags = flags;
        current->fds[fd].aux0 = ent.first_cluster;
        current->fds[fd].aux1 = ent.attr;
        for (int j = 0; j < 63; j++) {
            current->fds[fd].name[j] = path[j];
            if (path[j] == '\0') break;
        }
        current->fds[fd].name[63] = '\0';
        return fd;
    }

    struct ramfs_file* rf = find_ramfs(path);
    if (rf) {
        if ((rf->mode & 0170000U) == KSTAT_MODE_DIR) return -1;
        if (want_trunc) {
            rf->size = 0;
            rf->mtime_sec = fs_now_sec();
            rf->ctime_sec = rf->mtime_sec;
        }
        current->fds[fd].type = FT_RAMFS;
        current->fds[fd].data = rf->data;
        current->fds[fd].size = rf->size;
        current->fds[fd].offset = 0;
        current->fds[fd].in_use = 1;
        current->fds[fd].flags = flags;
        for(int j=0; j<63; j++) {
            current->fds[fd].name[j] = rf->name[j];
            if(rf->name[j] == '\0') break;
        }
        return fd;
    }

    if (want_creat) {
        rf = create_ramfs(path, (uint32_t)mode);
        if (rf) {
            current->fds[fd].type = FT_RAMFS;
            current->fds[fd].data = rf->data;
            current->fds[fd].size = 0;
            current->fds[fd].offset = 0;
            current->fds[fd].in_use = 1;
            current->fds[fd].flags = flags;
            for(int j=0; j<63; j++) {
                current->fds[fd].name[j] = rf->name[j];
                if(rf->name[j] == '\0') break;
            }
            return fd;
        }
    }

    {
        void* tar_data = 0;
        size_t tar_size = 0;
        uint32_t tar_mode = 0;
        if (fs_try_active_root_lookup(path, &tar_data, &tar_size, &tar_mode) == 0) {
            current->fds[fd].type = FT_TAR;
            current->fds[fd].data = tar_data;
            current->fds[fd].size = tar_size;
            current->fds[fd].offset = 0;
            current->fds[fd].in_use = 1;
            current->fds[fd].flags = flags;
            for (int j = 0; j < 63; j++) {
                current->fds[fd].name[j] = path[j];
                if (path[j] == '\0') break;
            }
            current->fds[fd].name[63] = '\0';
            return fd;
        }
    }

    if (fs_root_is_usb_only()) return -1;

    if (!module_request.response) return -1;

    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file* m = module_request.response->modules[i];
        if (strcmp_suffix_fs(m->path, path)) {
            current->fds[fd].type = FT_MODULE;
            current->fds[fd].data = m->address;
            current->fds[fd].size = m->size;
            current->fds[fd].offset = 0;
            current->fds[fd].in_use = 1;
            current->fds[fd].flags = flags;
            for (int j = 0; j < 63; j++) {
                current->fds[fd].name[j] = path[j];
                if (path[j] == '\0') break;
            }
            current->fds[fd].name[63] = '\0';
            return fd;
        }
        if (strcmp_suffix_fs(m->path, ".tar")) {
            uint8_t* ptr = (uint8_t*)m->address;
            uint8_t* end = ptr + m->size;
            while (ptr < end) {
                struct tar_header* h = (struct tar_header*)ptr;
                if (h->name[0] == '\0') break;
                uint64_t file_size = oct2int(h->size, 12);
                if (tar_name_matches(h->name, path)) {
                    current->fds[fd].type = FT_TAR;
                    current->fds[fd].data = ptr + 512;
                    current->fds[fd].size = file_size;
                    current->fds[fd].offset = 0;
                    current->fds[fd].in_use = 1;
                    current->fds[fd].flags = flags;
                    for (int j = 0; j < 63; j++) {
                        current->fds[fd].name[j] = path[j];
                        if (path[j] == '\0') break;
                    }
                    current->fds[fd].name[63] = '\0';
                    return fd;
                }
                ptr += 512 + ((file_size + 511) & ~511);
            }
        }
    }
    return -1;
}

int sys_openat(int dirfd, const char* path, int flags, int mode) {
    char resolved_path[256];
    if (resolve_dirfd_path(dirfd, path, resolved_path, sizeof(resolved_path)) < 0) return -1;
    return sys_open(resolved_path, flags, mode);
}

int64_t sys_write(int fd, const void* buf, size_t count) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (fd >= 0 && fd < MAX_FDS && current->fds[fd].in_use && current->fds[fd].type == FT_CONSOLE) {
        extern int64_t sys_write_serial(const char* buf, size_t count);
        return sys_write_serial((const char*)buf, count);
    }
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    file_descriptor_t* f = &current->fds[fd];

    if (f->type == FT_PIPE) {
        pipe_t* pipe = (pipe_t*)f->data;
        size_t written = 0;
        const char* src = (const char*)buf;
        while (written < count) {
            struct task* reader_to_wake = 0;
            uint64_t flags = spin_lock_irqsave(&pipe->lock);
            if (pipe->count == PIPE_BUF_SIZE) {
                task_mark_sleeping(current);
                pipe_set_waiter(&pipe->write_waiter, current);
                spin_unlock_irqrestore(&pipe->lock, flags);
                kernel_yield();
                flags = spin_lock_irqsave(&pipe->lock);
                pipe_clear_waiter(&pipe->write_waiter, current);
                spin_unlock_irqrestore(&pipe->lock, flags);
                continue;
            }
            while (written < count && pipe->count < PIPE_BUF_SIZE) {
                pipe->buffer[pipe->write_pos] = src[written];
                pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUF_SIZE;
                pipe->count++;
                written++;
            }
            reader_to_wake = pipe_take_waiter_locked(&pipe->read_waiter);
            spin_unlock_irqrestore(&pipe->lock, flags);
            pipe_wake_task(reader_to_wake);
        }
        return (int64_t)written;
    }

    if (f->type == FT_SOCKET) {
        return net_socket_write_fd(f, buf, count);
    }

    if (f->type != FT_RAMFS) return -1;
    if (f->offset + count > RAMFS_FILE_SIZE) count = RAMFS_FILE_SIZE - f->offset;
    uint8_t* dest = (uint8_t*)f->data + f->offset;
    const uint8_t* src = (const uint8_t*)buf;
    for (size_t i = 0; i < count; i++) dest[i] = src[i];
    f->offset += count;
    if (f->offset > f->size) {
        f->size = f->offset;
        struct ramfs_file* rf = find_ramfs(f->name);
        if (rf) {
            rf->size = f->size;
            rf->mtime_sec = fs_now_sec();
            rf->ctime_sec = rf->mtime_sec;
        }
    }
    return (int64_t)count;
}

int64_t sys_read(int fd, void* buf, size_t count) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (fd >= 0 && fd < MAX_FDS && current->fds[fd].in_use && current->fds[fd].type == FT_CONSOLE) {
        extern int kb_read(char* buf, int count);
        extern void kb_set_waiter(struct task* t);
        extern void kb_clear_waiter(struct task* t);
        extern int64_t sys_write_serial(const char* buf, size_t count);
        int read_bytes = 0;
        while (read_bytes == 0) {
            read_bytes = kb_read((char*)buf, count);
            if (read_bytes == 0) {
                task_mark_sleeping(current);
                kb_set_waiter(current);
                kernel_yield();
                kb_clear_waiter(current);
            }
        }
        if (fd == 0 && read_bytes > 0) {
            const char* src = (const char*)buf;
            for (int i = 0; i < read_bytes; i++) {
                if (src[i] == '\r' || src[i] == '\n') {
                    sys_write_serial("\r\n", 2);
                } else if (src[i] == '\b' || src[i] == 0x7F) {
                    sys_write_serial("\b \b", 3);
                } else {
                    sys_write_serial(&src[i], 1);
                }
            }
        }
        return read_bytes;
    }
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    file_descriptor_t* f = &current->fds[fd];

    if (f->type == FT_PIPE) {
        pipe_t* pipe = (pipe_t*)f->data;
        while (1) {
            size_t to_read;
            struct task* writer_to_wake = 0;
            uint64_t flags = spin_lock_irqsave(&pipe->lock);
            if (pipe->count == 0 && pipe->ref_count < 2) {
                spin_unlock_irqrestore(&pipe->lock, flags);
                return 0; // 書き込み側が閉じられた
            }
            if (pipe->count == 0) {
                task_mark_sleeping(current);
                pipe_set_waiter(&pipe->read_waiter, current);
                spin_unlock_irqrestore(&pipe->lock, flags);
                kernel_yield();
                flags = spin_lock_irqsave(&pipe->lock);
                pipe_clear_waiter(&pipe->read_waiter, current);
                spin_unlock_irqrestore(&pipe->lock, flags);
                continue;
            }
            to_read = (count > pipe->count) ? pipe->count : count;
            char* dest = (char*)buf;
            for (size_t i = 0; i < to_read; i++) {
                dest[i] = pipe->buffer[pipe->read_pos];
                pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUF_SIZE;
                pipe->count--;
            }
            writer_to_wake = pipe_take_waiter_locked(&pipe->write_waiter);
            spin_unlock_irqrestore(&pipe->lock, flags);
            pipe_wake_task(writer_to_wake);
            return (int64_t)to_read;
        }
    }

    if (f->type == FT_SOCKET) {
        return net_socket_read_fd(f, buf, count);
    }

    if (f->type == FT_USB) {
        struct fat_boot_info info;
        uint8_t sector[4096];
        size_t done = 0;
        uint32_t cluster;
        uint32_t cluster_size;
        size_t skip;
        if (usb_load_fat_boot(&info) < 0) return -1;
        cluster = f->aux0;
        cluster_size = (uint32_t)info.bytes_per_sector * info.sectors_per_cluster;
        if (cluster_size > sizeof(sector)) return -1;
        skip = f->offset;
        while (skip >= cluster_size && cluster >= 2 && cluster < 0x0FFFFFF8U) {
            cluster = fat_read_next_cluster(&info, cluster);
            skip -= cluster_size;
        }
        while (done < count && f->offset < f->size && cluster >= 2 && cluster < 0x0FFFFFF8U) {
            size_t chunk;
            uint32_t lba = fat_cluster_to_lba(&info, cluster);
            if (usb_read_blocks_safe(lba, sector, info.sectors_per_cluster) < 0) return (done > 0) ? (int64_t)done : -1;
            chunk = cluster_size - skip;
            if (chunk > count - done) chunk = count - done;
            if (chunk > f->size - f->offset) chunk = f->size - f->offset;
            for (size_t i = 0; i < chunk; i++) ((uint8_t*)buf)[done + i] = sector[skip + i];
            done += chunk;
            f->offset += chunk;
            skip = 0;
            if (done >= count || f->offset >= f->size) break;
            cluster = fat_read_next_cluster(&info, cluster);
        }
        return (int64_t)done;
    }

    if (f->type == FT_DIR) return -1;

    if (f->offset >= f->size) return 0;
    size_t remaining = f->size - f->offset;
    size_t to_read = (count > remaining) ? remaining : count;
    char* dest = (char*)buf;
    char* src = (char*)f->data + f->offset;
    for (size_t i = 0; i < to_read; i++) dest[i] = src[i];
    f->offset += to_read;
    if (f->type == FT_RAMFS) {
        struct ramfs_file* rf = find_ramfs(f->name);
        if (rf) rf->atime_sec = fs_now_sec();
    }
    return (int64_t)to_read;
}

int sys_close(int fd) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    
    if (current->fds[fd].type == FT_PIPE) {
        pipe_t* pipe = (pipe_t*)current->fds[fd].data;
        struct task* reader_to_wake;
        struct task* writer_to_wake;
        int free_pipe = 0;
        uint64_t flags = spin_lock_irqsave(&pipe->lock);
        pipe->ref_count--;
        reader_to_wake = pipe_take_waiter_locked(&pipe->read_waiter);
        writer_to_wake = pipe_take_waiter_locked(&pipe->write_waiter);
        if (pipe->ref_count == 0) {
            free_pipe = 1;
        }
        spin_unlock_irqrestore(&pipe->lock, flags);
        pipe_wake_task(reader_to_wake);
        pipe_wake_task(writer_to_wake);
        if (free_pipe) {
            pmm_free((void*)VIRT_TO_PHYS((uint64_t)pipe), 1);
        }
    } else if (current->fds[fd].type == FT_SOCKET) {
        (void)net_socket_close_fd(&current->fds[fd]);
    } else if (current->fds[fd].type == FT_DIR) {
        if (current->fds[fd].data) {
            pmm_free((void*)VIRT_TO_PHYS((uint64_t)current->fds[fd].data), (int)current->fds[fd].aux0);
        }
    }

    current->fds[fd].in_use = 0;
    current->fds[fd].data = NULL;
    current->fds[fd].size = 0;
    current->fds[fd].offset = 0;
    current->fds[fd].name[0] = '\0';
    return 0;
}

int sys_dup2(int oldfd, int newfd) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (oldfd < 0 || oldfd >= MAX_FDS || !current->fds[oldfd].in_use) return -1;
    if (newfd < 0 || newfd >= MAX_FDS) return -1;
    if (oldfd == newfd) return newfd;

    if (current->fds[newfd].in_use) {
        sys_close(newfd);
    }

    current->fds[newfd] = current->fds[oldfd];
    if (current->fds[newfd].type == FT_PIPE) {
        pipe_t* pipe = (pipe_t*)current->fds[newfd].data;
        uint64_t flags = spin_lock_irqsave(&pipe->lock);
        pipe->ref_count++;
        spin_unlock_irqrestore(&pipe->lock, flags);
    } else if (current->fds[newfd].type == FT_SOCKET) {
        net_socket_dup_fd(&current->fds[newfd]);
    }
    return newfd;
}

int sys_fcntl(int fd, int cmd, uint64_t arg) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;

    switch (cmd) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            int minfd = (int)arg;
            if (minfd < 0) return -1;
            for (int newfd = minfd; newfd < MAX_FDS; newfd++) {
                if (!current->fds[newfd].in_use) {
                    if (sys_dup2(fd, newfd) < 0) return -1;
                    current->fds[newfd].fd_flags =
                        (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
                    return newfd;
                }
            }
            return -1;
        }
        case F_GETFD:
            return current->fds[fd].fd_flags;
        case F_SETFD:
            current->fds[fd].fd_flags = (int)arg & FD_CLOEXEC;
            return 0;
        case F_GETFL:
            return current->fds[fd].flags;
        case F_SETFL:
            current->fds[fd].flags = (current->fds[fd].flags & ORTH_O_ACCMODE)
                                   | ((int)arg & ~ORTH_O_ACCMODE);
            return 0;
        default:
            return -1;
    }
}

int sys_pipe(int pipefd[2]) {
    struct task* current = get_current_task();
    if (!current) return -1;

    int fd1 = -1, fd2 = -1;
    for (int i = 3; i < MAX_FDS; i++) {
        if (!current->fds[i].in_use) {
            if (fd1 == -1) fd1 = i;
            else if (fd2 == -1) {
                fd2 = i;
                break;
            }
        }
    }
    if (fd1 == -1 || fd2 == -1) return -1;

    void* phys = pmm_alloc(1);
    if (!phys) return -1;
    pipe_t* pipe = (pipe_t*)PHYS_TO_VIRT(phys);
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->count = 0;
    pipe->ref_count = 2;
    pipe->read_waiter = 0;
    pipe->write_waiter = 0;
    spinlock_init(&pipe->lock);

    current->fds[fd1].type = FT_PIPE;
    current->fds[fd1].data = pipe;
    current->fds[fd1].in_use = 1;
    current->fds[fd1].flags = O_RDONLY;
    current->fds[fd1].name[0] = '\0';

    current->fds[fd2].type = FT_PIPE;
    current->fds[fd2].data = pipe;
    current->fds[fd2].in_use = 1;
    current->fds[fd2].flags = O_WRONLY;
    current->fds[fd2].name[0] = '\0';

    pipefd[0] = fd1;
    pipefd[1] = fd2;
    return 0;
}

int sys_pipe2(int pipefd[2], int flags) {
    int ret;
    if (flags & ~(ORTH_LINUX_O_CLOEXEC | ORTH_LINUX_O_NONBLOCK)) return -1;
    ret = sys_pipe(pipefd);
    if (ret < 0) return ret;
    if (flags & ORTH_LINUX_O_CLOEXEC) {
        struct task* current = get_current_task();
        if (current) {
            current->fds[pipefd[0]].fd_flags |= FD_CLOEXEC;
            current->fds[pipefd[1]].fd_flags |= FD_CLOEXEC;
        }
    }
    return 0;
}

int sys_fstat(int fd, struct kstat* st) {
    struct task* current = get_current_task();
    if (!current || !st) return -1;
    
    if (fd == 0 || fd == 1 || fd == 2) {
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_CHR), 0);
        st->dev = FS_DEV_CONSOLE;
        st->ino = (uint64_t)fd + 1U;
        st->rdev = FS_DEV_CONSOLE;
        return 0;
    }
    
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    
    file_descriptor_t* f = &current->fds[fd];
    if (f->type == FT_CONSOLE) {
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_CHR), 0);
        st->dev = FS_DEV_CONSOLE;
        st->ino = (uint64_t)fd + 1U;
        st->rdev = FS_DEV_CONSOLE;
        return 0;
    }
    if (f->type == FT_PIPE) {
        kstat_set_defaults(st, KSTAT_MODE_FILE | 0600U, 0);
        st->dev = FS_DEV_PIPE;
        st->ino = ((uint64_t)(uintptr_t)f->data) >> 4;
        return 0;
    }
    if (f->type == FT_SOCKET) {
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_CHR), 0);
        st->dev = FS_DEV_PIPE;
        st->ino = ((uint64_t)(uintptr_t)f->data) >> 4;
        return 0;
    }
    if (f->type == FT_DIR) {
        if (f->name[0]) return sys_stat(f->name, st);
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_DIR), (int64_t)f->size);
        st->dev = FS_DEV_SYNTH;
        st->ino = (uint64_t)fd + 1U;
        return 0;
    }
    if (f->type == FT_USB) {
        if (f->name[0]) return sys_stat(f->name, st);
        kstat_set_defaults(st,
                           fs_default_mode_for_type((f->aux1 & 0x10U) ? KSTAT_MODE_DIR : KSTAT_MODE_FILE),
                           (int64_t)f->size);
        st->dev = FS_DEV_USB_FAT;
        st->ino = f->name[0] ? fs_hash_name(f->name) : ((uint64_t)f->aux0 + 1U);
        return 0;
    }
    if ((f->type == FT_RAMFS || f->type == FT_TAR || f->type == FT_MODULE || f->type == FT_USBROOT) && f->name[0]) {
        return sys_stat(f->name, st);
    }
    kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_FILE), (int64_t)f->size);
    return 0;
}

int sys_getdents(int fd, struct orth_dirent* dirp, size_t count) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    size_t remaining;
    size_t to_copy;
    if (!current || !dirp) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    f = &current->fds[fd];
    if (f->type != FT_DIR) return -1;
    if (f->offset >= f->size) return 0;
    remaining = f->size - f->offset;
    to_copy = (count > remaining) ? remaining : count;
    for (size_t i = 0; i < to_copy; i++) {
        ((uint8_t*)dirp)[i] = ((uint8_t*)f->data)[f->offset + i];
    }
    f->offset += to_copy;
    return (int)to_copy;
}

struct linux_dirent_compat {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

static uint8_t dirent_type_from_mode(uint32_t mode) {
    if ((mode & 0170000U) == KSTAT_MODE_DIR) return 4;
    if ((mode & 0170000U) == KSTAT_MODE_FILE) return 8;
    if ((mode & 0170000U) == KSTAT_MODE_CHR) return 2;
    return 0;
}

int sys_getdents64(int fd, void* dirp, size_t count) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    struct orth_dirent* src;
    uint8_t* out = (uint8_t*)dirp;
    size_t out_used = 0;
    size_t index;

    if (!current || !dirp) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    f = &current->fds[fd];
    if (f->type != FT_DIR) return -1;
    if (f->offset >= f->size) return 0;

    src = (struct orth_dirent*)f->data;
    index = f->offset / sizeof(struct orth_dirent);

    while ((index + 1) * sizeof(struct orth_dirent) <= f->size) {
        struct linux_dirent_compat ent;
        size_t name_len = 0;
        size_t reclen;
        size_t next_off;

        for (size_t i = 0; i < sizeof(ent); i++) {
            ((uint8_t*)&ent)[i] = 0;
        }
        while (src[index].name[name_len] && name_len + 1 < sizeof(ent.d_name)) {
            ent.d_name[name_len] = src[index].name[name_len];
            name_len++;
        }
        ent.d_name[name_len] = '\0';
        ent.d_ino = (uint64_t)index + 1;
        next_off = (index + 1) * sizeof(struct orth_dirent);
        ent.d_off = (int64_t)next_off;
        ent.d_type = dirent_type_from_mode(src[index].mode);
        reclen = offsetof(struct linux_dirent_compat, d_name) + name_len + 1;
        reclen = (reclen + 7U) & ~7U;
        ent.d_reclen = (uint16_t)reclen;

        if (out_used + reclen > count) break;
        for (size_t i = 0; i < reclen; i++) {
            out[out_used + i] = ((uint8_t*)&ent)[i];
        }
        out_used += reclen;
        index++;
    }

    f->offset = index * sizeof(struct orth_dirent);
    return (int)out_used;
}

int sys_stat(const char* path, struct kstat* st) {
    const char* mount_subpath = 0;
    struct mount_point* mount = 0;
    char resolved_path[256];
    if (!st || !path) return -1;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    path = normalize_fs_path(resolved_path);
    mount = fs_resolve_mount(path, &mount_subpath);

    if (*path == '\0') {
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_DIR), 0);
        st->dev = FS_DEV_SYNTH;
        st->ino = 1;
        return 0;
    }

    if (fs_is_mount_dir(path)) {
        kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_DIR), 0);
        st->dev = FS_DEV_SYNTH;
        st->ino = fs_hash_name(path);
        return usb_block_device_ready() ? 0 : -1;
    }
    
    struct ramfs_file* rf = find_ramfs(path);
    if (rf) {
        kstat_from_ramfs(st, rf);
        return 0;
    }

    if (mount && mount->type == MOUNT_USB_FAT) {
        struct fat_dir_entry_info ent;
        if (fat_resolve_path(path, &ent) < 0) return -1;
        kstat_set_defaults(st,
                           fs_default_mode_for_type((ent.attr & 0x10U) ? KSTAT_MODE_DIR : KSTAT_MODE_FILE),
                           (int64_t)ent.size);
        st->dev = FS_DEV_USB_FAT;
        st->ino = fs_hash_name(path);
        return 0;
    }

    {
        if (fs_try_active_root_stat(path, st) == 0) {
            return 0;
        }
        if (fs_root_is_usb_only()) return -1;
    }
    
    if (module_request.response) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file* m = module_request.response->modules[i];
            if (strcmp_suffix_fs(m->path, path)) {
                kstat_set_defaults(st, fs_default_mode_for_type(KSTAT_MODE_FILE), (int64_t)m->size);
                st->dev = FS_DEV_MODULE;
                st->ino = fs_hash_name(path);
                return 0;
            }
            if (strcmp_suffix_fs(m->path, ".tar")) {
                uint8_t* ptr = (uint8_t*)m->address;
                uint8_t* end = ptr + m->size;
                while (ptr < end) {
                    struct tar_header* h = (struct tar_header*)ptr;
                    if (h->name[0] == '\0') break;
                    uint64_t file_size = oct2int(h->size, 12);
                    if (tar_name_matches(h->name, path)) {
                        kstat_from_tar_header(st, h, file_size);
                        return 0;
                    }
                    ptr += 512 + ((file_size + 511) & ~511);
                }
            }
        }
    }
    return -1; // File not found
}

int sys_fstatat(int dirfd, const char* path, struct kstat* st, int flags) {
    char resolved_path[256];
    (void)flags;
    if (resolve_dirfd_path(dirfd, path, resolved_path, sizeof(resolved_path)) < 0) return -1;
    return sys_stat(resolved_path, st);
}

int64_t sys_lseek(int fd, int64_t offset, int whence) {
    struct task* current = get_current_task();
    if (!current) return -1;
    
    if (fd == 0 || fd == 1 || fd == 2) return 0;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    
    file_descriptor_t* f = &current->fds[fd];
    int64_t new_offset = 0;
    
    if (whence == 0) { // SEEK_SET
        new_offset = offset;
    } else if (whence == 1) { // SEEK_CUR
        new_offset = f->offset + offset;
    } else if (whence == 2) { // SEEK_END
        new_offset = f->size + offset;
    } else {
        return -1;
    }
    
    if (new_offset < 0) return -1;
    f->offset = new_offset;
    return new_offset;
}

int sys_unlink(const char* path) {
    char resolved_path[256];
    if (!path) return -1;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    path = normalize_fs_path(resolved_path);
    
    // 現在の簡易実装では Ramfs のファイルのみ削除可能とする
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (ramfs_table[i].in_use && strcmp_exact(ramfs_table[i].name, path)) {
            if ((ramfs_table[i].mode & 0170000U) == KSTAT_MODE_DIR) return -1;
            ramfs_table[i].in_use = 0;
            if (ramfs_table[i].data) {
                // 16ページ確保していたものを解放
                void* phys_addr = (void*)VIRT_TO_PHYS((uint64_t)ramfs_table[i].data);
                pmm_free(phys_addr, 16);
                ramfs_table[i].data = NULL;
            }
            ramfs_table[i].size = 0;
            ramfs_table[i].mode = 0;
            ramfs_table[i].uid = 0;
            ramfs_table[i].gid = 0;
            ramfs_table[i].nlink = 0;
            ramfs_table[i].atime_sec = 0;
            ramfs_table[i].mtime_sec = 0;
            ramfs_table[i].ctime_sec = 0;
            ramfs_table[i].name[0] = '\0';
            return 0; // 成功
        }
    }
    
    return -1; // ファイルが見つからないか、TAR 内など削除できないファイル
}

int sys_rename(const char* oldpath, const char* newpath) {
    char old_resolved[256];
    char new_resolved[256];
    const char* old_norm;
    const char* new_norm;
    struct ramfs_file* rf;
    if (!oldpath || !newpath) return -1;
    resolve_task_path(oldpath, old_resolved, sizeof(old_resolved));
    resolve_task_path(newpath, new_resolved, sizeof(new_resolved));
    old_norm = normalize_fs_path(old_resolved);
    new_norm = normalize_fs_path(new_resolved);
    if (*old_norm == '\0' || *new_norm == '\0') return -1;
    if (strcmp_exact(old_norm, new_norm)) return 0;
    if (find_ramfs(new_norm)) return -1;
    rf = find_ramfs(old_norm);
    if (!rf) return -1;
    int i = 0;
    for (; new_norm[i] && i + 1 < (int)sizeof(rf->name); i++) {
        rf->name[i] = new_norm[i];
    }
    rf->name[i] = '\0';
    rf->ctime_sec = fs_now_sec();
    return 0;
}

int sys_chmod(const char* path, uint32_t mode) {
    char resolved_path[256];
    const char* norm;
    struct ramfs_file* rf;
    if (!path) return -1;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    norm = normalize_fs_path(resolved_path);
    if (*norm == '\0') return -1;
    rf = find_ramfs(norm);
    if (rf) {
        rf->mode = (rf->mode & 0170000U) | (mode & 07777U);
        rf->ctime_sec = fs_now_sec();
        return 0;
    }
    return -1;
}

int sys_mkdir(const char* path, int mode) {
    char resolved_path[256];
    const char* norm;
    (void)mode;
    if (!path || path[0] == '\0') return -1;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    norm = normalize_fs_path(resolved_path);
    if (*norm == '\0') return -1;
    if (contains_char(norm, '/')) return -1;
    if (find_ramfs(norm)) return -1;
    return create_ramfs_dir(norm, (uint32_t)mode) ? 0 : -1;
}

int sys_mkdirat(int dirfd, const char* path, int mode) {
    char resolved_path[256];
    if (resolve_dirfd_path(dirfd, path, resolved_path, sizeof(resolved_path)) < 0) return -1;
    return sys_mkdir(resolved_path, mode);
}

int sys_rmdir(const char* path) {
    char resolved_path[256];
    const char* norm;
    if (!path || path[0] == '\0') return -1;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    norm = normalize_fs_path(resolved_path);
    if (*norm == '\0') return -1;
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (!ramfs_table[i].in_use) continue;
        if (!strcmp_exact(ramfs_table[i].name, norm)) continue;
        if ((ramfs_table[i].mode & 0170000U) != KSTAT_MODE_DIR) return -1;
        ramfs_table[i].in_use = 0;
        ramfs_table[i].size = 0;
        ramfs_table[i].data = NULL;
        ramfs_table[i].mode = 0;
        ramfs_table[i].uid = 0;
        ramfs_table[i].gid = 0;
        ramfs_table[i].nlink = 0;
        ramfs_table[i].atime_sec = 0;
        ramfs_table[i].mtime_sec = 0;
        ramfs_table[i].ctime_sec = 0;
        ramfs_table[i].name[0] = '\0';
        return 0;
    }
    return -1;
}

int sys_unlinkat(int dirfd, const char* path, int flags) {
    char resolved_path[256];
    if (resolve_dirfd_path(dirfd, path, resolved_path, sizeof(resolved_path)) < 0) return -1;
    if (flags & ORTH_AT_REMOVEDIR) return sys_rmdir(resolved_path);
    return sys_unlink(resolved_path);
}

int sys_chdir(const char* path) {
    struct task* current = get_current_task();
    struct kstat st;
    char resolved_path[256];
    const char* norm;
    size_t i = 0;
    if (!current || !path || path[0] == '\0') return -1;
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    if (sys_stat(resolved_path, &st) < 0) return -1;
    if ((st.mode & 0170000U) != KSTAT_MODE_DIR) return -1;
    norm = normalize_fs_path(resolved_path);
    current->cwd[i++] = '/';
    while (*norm && i + 1 < sizeof(current->cwd)) {
        current->cwd[i++] = *norm++;
    }
    current->cwd[i] = '\0';
    return 0;
}

int sys_fchdir(int fd) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    f = &current->fds[fd];
    if (f->type != FT_DIR) return -1;
    if (f->name[0] == '\0') return -1;
    return sys_chdir(f->name);
}

int sys_getcwd(char* buf, size_t size) {
    struct task* current = get_current_task();
    size_t i = 0;
    if (!current || !buf || size == 0) return -1;
    while (current->cwd[i] && i + 1 < size) {
        buf[i] = current->cwd[i];
        i++;
    }
    if (current->cwd[i] != '\0' && i + 1 >= size) return -1;
    buf[i] = '\0';
    return 0;
}

int fs_get_file_data(const char* path, void** data, size_t* size) {
    char resolved_path[256];
    resolve_task_path(path, resolved_path, sizeof(resolved_path));
    path = normalize_fs_path(resolved_path);

    struct ramfs_file* rf = find_ramfs(path);
    if (rf) {
        *data = rf->data;
        *size = rf->size;
        return 0;
    }

    if (fs_try_active_root_lookup(path, data, size, 0) == 0) {
        return 0;
    }
    if (fs_root_is_usb_only()) return -1;

    if (module_request.response) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file* m = module_request.response->modules[i];
            if (strcmp_suffix_fs(m->path, path)) {
                *data = m->address;
                *size = m->size;
                return 0;
            }
            if (strcmp_suffix_fs(m->path, ".tar")) {
                uint8_t* ptr = (uint8_t*)m->address;
                uint8_t* end = ptr + m->size;
                while (ptr < end) {
                    struct tar_header* h = (struct tar_header*)ptr;
                    if (h->name[0] == '\0') break;
                    uint64_t file_size = oct2int(h->size, 12);
                    if (tar_name_matches(h->name, path)) {
                        *data = ptr + 512;
                        *size = file_size;
                        return 0;
                    }
                    ptr += 512 + ((file_size + 511) & ~511);
                }
            }
        }
    }
    return -1;
}

void sys_ls(void) {
    puts("--- Files in Ramfs ---\n");
    for (int i = 0; i < MAX_RAMFS_FILES; i++) {
        if (ramfs_table[i].in_use) {
            puts(ramfs_table[i].name);
            puts(" (Ramfs, size: 0x"); puthex(ramfs_table[i].size); puts(")\n");
        }
    }
    if (g_root_source == ROOT_SOURCE_USB && g_usb_root_tar && g_usb_root_tar_size > 0) {
        fs_list_archive_entries(g_usb_root_tar, g_usb_root_tar_size, "--- Files in Active Root ---\n");
    } else if (g_root_source == ROOT_SOURCE_USB && g_usb_root_tar_file_ready) {
        fs_list_usb_root_tar_entries("--- Files in Active Root ---\n");
    } else if (module_request.response) {
        puts("--- Files in TAR/Modules ---\n");
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file* m = module_request.response->modules[i];
            if (strcmp_suffix_fs(m->path, ".tar")) {
                uint8_t* ptr = (uint8_t*)m->address;
                uint8_t* end = ptr + m->size;
                while (ptr < end) {
                    struct tar_header* h = (struct tar_header*)ptr;
                    if (h->name[0] == '\0') break;
                    uint64_t file_size = oct2int(h->size, 12);
                    puts(h->name);
                    puts(" (TAR, size: 0x"); puthex(file_size); puts(")\n");
                    ptr += 512 + ((file_size + 511) & ~511);
                }
            } else {
                puts(m->path); puts(" (Module)\n");
            }
        }
    }
    if (usb_block_device_ready()) {
        for (size_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
            if (!g_mounts[i].active) continue;
            puts(g_mounts[i].path);
            puts(" (");
            if (g_mounts[i].type == MOUNT_USB_FAT) puts("USB mount");
            else puts("mount");
            puts(")\n");
        }
    }
}
