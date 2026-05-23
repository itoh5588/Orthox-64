#include "procfs.h"
#include "fs.h"
#include "pmm.h"
#include "vmm.h"
#include "lapic.h"
#include "vfs.h"
#include "string.h"

#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef EROFS
#define EROFS 30
#endif

#define PROCFS_BUF_PAGES 1
#define PROCFS_BUF_SIZE  (PROCFS_BUF_PAGES * PAGE_SIZE)

enum procfs_node {
    PROCFS_UPTIME,
    PROCFS_MEMINFO,
    PROCFS_MOUNTS
};

struct procfs_entry {
    const char* name;
    enum procfs_node node;
};

static const struct procfs_entry g_proc_entries[] = {
    { "uptime",  PROCFS_UPTIME  },
    { "meminfo", PROCFS_MEMINFO },
    { "mounts",  PROCFS_MOUNTS  },
};

static size_t append_str(char* buf, size_t cap, size_t off, const char* s) {
    while (*s && off + 1 < cap) buf[off++] = *s++;
    return off;
}

static size_t append_u64(char* buf, size_t cap, size_t off, uint64_t v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10U)); v /= 10U; }
    while (n > 0 && off + 1 < cap) buf[off++] = tmp[--n];
    return off;
}

static size_t append_u64_pad3(char* buf, size_t cap, size_t off, uint64_t v) {
    char tmp[4];
    tmp[0] = (char)('0' + (v / 100U) % 10U);
    tmp[1] = (char)('0' + (v / 10U) % 10U);
    tmp[2] = (char)('0' + v % 10U);
    for (int i = 0; i < 3 && off + 1 < cap; i++) buf[off++] = tmp[i];
    return off;
}

static size_t build_uptime(char* buf, size_t cap) {
    uint64_t ms = lapic_get_ticks_ms();
    uint64_t sec = ms / 1000U;
    uint64_t frac = ms % 1000U;
    size_t off = 0;
    off = append_str(buf, cap, off, "uptime_sec ");
    off = append_u64(buf, cap, off, sec);
    off = append_str(buf, cap, off, ".");
    off = append_u64_pad3(buf, cap, off, frac);
    off = append_str(buf, cap, off, "\n");
    off = append_str(buf, cap, off, "uptime_ms ");
    off = append_u64(buf, cap, off, ms);
    off = append_str(buf, cap, off, "\n");
    return off;
}

static size_t emit_kv(char* buf, size_t cap, size_t off, const char* key, uint64_t v) {
    off = append_str(buf, cap, off, key);
    off = append_str(buf, cap, off, " ");
    off = append_u64(buf, cap, off, v);
    off = append_str(buf, cap, off, "\n");
    return off;
}

static size_t build_meminfo(char* buf, size_t cap) {
    uint64_t page_size = PAGE_SIZE;
    uint64_t total_pages = pmm_get_total_pages();
    uint64_t free_pages = pmm_get_free_pages();
    uint64_t used_pages = pmm_get_allocated_pages();
    size_t off = 0;
    off = emit_kv(buf, cap, off, "page_size",       page_size);
    off = emit_kv(buf, cap, off, "total_pages",     total_pages);
    off = emit_kv(buf, cap, off, "free_pages",      free_pages);
    off = emit_kv(buf, cap, off, "used_pages",      used_pages);
    off = emit_kv(buf, cap, off, "total_mem_bytes", total_pages * page_size);
    off = emit_kv(buf, cap, off, "free_mem_bytes",  free_pages * page_size);
    off = emit_kv(buf, cap, off, "used_mem_bytes",  used_pages * page_size);
    return off;
}

static size_t build_mounts(char* buf, size_t cap) {
    const struct vfs_mountpoint* mounts[VFS_MAX_MOUNTPOINTS];
    size_t count = vfs_list_mountpoints(mounts, VFS_MAX_MOUNTPOINTS);
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        const char* kind_name = "unknown";
        switch (mounts[i]->kind) {
            case VFS_MOUNT_USB_FAT:  kind_name = "usb-fat"; break;
            case VFS_MOUNT_IMAGE_FS: kind_name = "image-fs"; break;
            case VFS_MOUNT_PROCFS:   kind_name = "procfs"; break;
            default: break;
        }
        off = append_str(buf, cap, off, "/");
        off = append_str(buf, cap, off, mounts[i]->path);
        off = append_str(buf, cap, off, " ");
        off = append_str(buf, cap, off, kind_name);
        off = append_str(buf, cap, off, "\n");
    }
    return off;
}

static void procfs_release_file(fs_file_t* file) {
    if (!file) return;
    if (file->private_data && file->aux0) {
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)file->private_data), (size_t)file->aux0);
        file->private_data = 0;
    }
}

static const fs_file_ops_t g_procfs_file_ops = {
    .release = procfs_release_file,
};

fs_file_t* procfs_open(const char* subpath, int flags) {
    if (!subpath || subpath[0] == '\0') return 0;
    if ((flags & 3) != 0) return 0; /* O_RDONLY only */

    enum procfs_node node;
    int found = 0;
    for (size_t i = 0; i < sizeof(g_proc_entries) / sizeof(g_proc_entries[0]); i++) {
        if (strcmp(subpath, g_proc_entries[i].name) == 0) {
            node = g_proc_entries[i].node;
            found = 1;
            break;
        }
    }
    if (!found) return 0;

    void* file_phys = pmm_alloc(1);
    if (!file_phys) return 0;
    fs_file_t* file = (fs_file_t*)PHYS_TO_VIRT(file_phys);
    memset(file, 0, sizeof(*file));
    file->ref_count = 1;
    file->type = FT_PROCFS;
    file->ops = &g_procfs_file_ops;

    void* buf_phys = pmm_alloc(PROCFS_BUF_PAGES);
    if (!buf_phys) {
        pmm_free(file_phys, 1);
        return 0;
    }
    char* buf = (char*)PHYS_TO_VIRT(buf_phys);
    memset(buf, 0, PROCFS_BUF_SIZE);

    size_t len = 0;
    switch (node) {
        case PROCFS_UPTIME:  len = build_uptime(buf, PROCFS_BUF_SIZE); break;
        case PROCFS_MEMINFO: len = build_meminfo(buf, PROCFS_BUF_SIZE); break;
        case PROCFS_MOUNTS:  len = build_mounts(buf, PROCFS_BUF_SIZE); break;
    }

    file->private_data = buf;
    file->aux0 = PROCFS_BUF_PAGES;
    file->aux1 = (uint32_t)node;
    file->size = len;
    file->offset = 0;
    const char* p = subpath;
    int j;
    for (j = 0; j < 63 && p[j]; j++) file->path[j] = p[j];
    file->path[j] = '\0';

    return file;
}
