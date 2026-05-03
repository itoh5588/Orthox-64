#include "retrofs.h"
#include "fs.h"
#include "pmm.h"
#include "storage.h"
#include "vmm.h"

extern void* memcpy(void* dest, const void* src, size_t n);
extern void* memset(void* s, int c, size_t n);

static struct retrofs_context g_retrofs;

static int strcmp_exact_retrofs(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int retrofs_dirent_name_exists(const struct orth_dirent* dirents, size_t count, const char* name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp_exact_retrofs(dirents[i].name, name)) return 1;
    }
    return 0;
}

static int path_component_len(const char* s) {
    int len = 0;
    while (s[len] && s[len] != '/') len++;
    return len;
}

#if 0
#include <stdarg.h>

extern int vsnprintf(char* dst, size_t size, const char* fmt, va_list ap);
extern int64_t sys_write_serial(const char* buf, size_t count);

static int cprintf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0) sys_write_serial(buf, len);
    return len;
}
#endif

static const char* normalize_retrofs_path(const char* path) {
    if (!path) return "";
    while (*path == '/') path++;
    if (path[0] == '.' && path[1] == '/') {
        path += 2;
        while (*path == '/') path++;
    }
    return path;
}

static void strncpy_retrofs(char* dst, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
}

static int retrofs_read_sectors(uint64_t lba, void* buf, size_t count) {
    if (!g_retrofs.mounted || !buf || count == 0) return -1;
    return storage_read_blocks(g_retrofs.device_name, g_retrofs.start_lba + lba, buf, count);
}

static int retrofs_write_sectors(uint64_t lba, const void* buf, size_t count) {
    if (!g_retrofs.mounted || !buf || count == 0) return -1;
    return storage_write_blocks(g_retrofs.device_name, g_retrofs.start_lba + lba, buf, count);
}

static int retrofs_bitmap_get(uint64_t sector) {
    uint8_t buf[RETROFS_SECTOR_SIZE];
    uint64_t map_sector = sector / (RETROFS_SECTOR_SIZE * 8);
    uint32_t byte_off = (sector % (RETROFS_SECTOR_SIZE * 8)) / 8;
    uint32_t bit_off = sector % 8;
    if (map_sector >= g_retrofs.desc.free_space_map_length) return 1; // Out of bounds = used
    if (retrofs_read_sectors(g_retrofs.desc.free_space_map_start + map_sector, buf, 1) < 0) return 1;
    return (buf[byte_off] >> bit_off) & 1;
}

static int retrofs_bitmap_set(uint64_t sector, int used) {
    uint8_t buf[RETROFS_SECTOR_SIZE];
    uint64_t map_sector = sector / (RETROFS_SECTOR_SIZE * 8);
    uint32_t byte_off = (sector % (RETROFS_SECTOR_SIZE * 8)) / 8;
    uint32_t bit_off = sector % 8;
    if (map_sector >= g_retrofs.desc.free_space_map_length) return -1;
    if (retrofs_read_sectors(g_retrofs.desc.free_space_map_start + map_sector, buf, 1) < 0) return -1;
    if (used) buf[byte_off] |= (1 << bit_off);
    else buf[byte_off] &= ~(1 << bit_off);
    return retrofs_write_sectors(g_retrofs.desc.free_space_map_start + map_sector, buf, 1);
}

static int64_t retrofs_alloc_sectors(uint64_t count) {
    uint64_t found = 0;
    uint64_t start = 0;
    // Skip sector 0 (desc block)
    for (uint64_t i = 1; i < g_retrofs.total_sectors; i++) {
        if (!retrofs_bitmap_get(i)) {
            if (found == 0) start = i;
            found++;
            if (found == count) {
                for (uint64_t j = 0; j < count; j++) {
                    retrofs_bitmap_set(start + j, 1);
                }
                return (int64_t)start;
            }
        } else {
            found = 0;
        }
    }
    return -1;
}

static uint32_t retrofs_mode_from_flags(uint32_t flags) {
    return (flags & RETROFS_FLAG_DIRECTORY) ? KSTAT_MODE_DIR | 0555U : KSTAT_MODE_FILE | 0444U;
}

static int retrofs_load_directory(uint64_t dir_sector,
                                  union retrofs_directory_entry** out_block,
                                  size_t* out_entry_count,
                                  uint64_t* out_continuation,
                                  uint64_t* out_pages) {
    uint8_t sector_buf[RETROFS_SECTOR_SIZE];
    union retrofs_directory_entry* header = (union retrofs_directory_entry*)sector_buf;
    struct retrofs_directory_start* start_entry;
    uint64_t sectors;
    uint64_t pages;
    void* phys;
    union retrofs_directory_entry* block;

    if (!out_block || !out_entry_count || !out_continuation || !out_pages) return -1;
    if (retrofs_read_sectors(dir_sector, sector_buf, 1) < 0) return -1;
    start_entry = &header[0].start;
    sectors = start_entry->sectors;
    if ((start_entry->flags & RETROFS_FLAG_DIR_START) == 0 || sectors == 0 || sectors > RETROFS_MAX_DIR_SECTORS) {
        return -1;
    }

    pages = (sectors * RETROFS_SECTOR_SIZE + PAGE_SIZE - 1U) / PAGE_SIZE;
    phys = pmm_alloc((int)pages);
    if (!phys) return -1;
    block = (union retrofs_directory_entry*)PHYS_TO_VIRT(phys);
    if (retrofs_read_sectors(dir_sector, block, (size_t)sectors) < 0) {
        pmm_free(phys, (int)pages);
        return -1;
    }

    *out_block = block;
    *out_entry_count = (size_t)sectors * 2U - 1U;
    *out_continuation = block[0].start.continuation;
    *out_pages = pages;
    return 0;
}

static int retrofs_find_in_directory(uint64_t dir_sector, const char* name,
                                     struct retrofs_directory_entry_inner* out_entry) {
    uint64_t current = dir_sector;
    uint32_t walked = 0;
    while (current != 0 && walked++ < (1U << 16)) {
        union retrofs_directory_entry* block = 0;
        union retrofs_directory_entry* entries;
        uint64_t continuation = 0;
        uint64_t pages = 0;
        size_t entry_count;
        if (retrofs_load_directory(current, &block, &entry_count, &continuation, &pages) < 0) return -1;
        entries = &block[1];
        for (size_t i = 0; i < entry_count; i++) {
            struct retrofs_directory_entry_inner* entry = &entries[i].entry;
            if (entry->filename[0] == '\0') continue; /* skip deleted/unused slots */
            // cprintf("find: checking '%s' == '%s'?\r\n", entry->filename, name);
            if (strcmp_exact_retrofs(entry->filename, name)) {
                if (out_entry) *out_entry = *entry;
                pmm_free((void*)VIRT_TO_PHYS((uint64_t)block), (int)pages);
                return 0;
            }
        }
        current = continuation;
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)block), (int)pages);
    }
    return -1;
}

static int retrofs_walk_path(const char* path, struct retrofs_directory_entry_inner* out_entry, int* out_is_root, uint64_t* out_parent_sector) {
    const char* norm = normalize_retrofs_path(path);
    uint64_t current_dir = g_retrofs.desc.root_directory;
    struct retrofs_directory_entry_inner current = {0};
    if (out_is_root) *out_is_root = 0;
    if (out_parent_sector) *out_parent_sector = 0;
    if (!g_retrofs.mounted) return -1;
    if (norm[0] == '\0') {
        if (out_is_root) *out_is_root = 1;
        return 0;
    }
    while (*norm) {
        if (out_parent_sector) *out_parent_sector = current_dir;
        const char* next = norm;
        char component[RETROFS_MAX_NAME];
        int len = path_component_len(norm);
        if (len <= 0 || len >= RETROFS_MAX_NAME) {
            return -1;
        }
        for (int i = 0; i < len; i++) component[i] = norm[i];
        component[len] = '\0';
        if (retrofs_find_in_directory(current_dir, component, &current) < 0) {
            return -1;
        }
        next += len;
        while (*next == '/') next++;
        if (*next != '\0') {
            if ((current.flags & RETROFS_FLAG_DIRECTORY) == 0) return -1;
            current_dir = current.sector_start;
        }
        norm = next;
    }
    if (out_entry) *out_entry = current;
    return 0;
}

static void get_filename_from_path(const char* path, char* out) {
    const char* norm = normalize_retrofs_path(path);
    const char* last = norm;
    const char* p = norm;
    while (*p) {
        if (*p == '/') last = p + 1;
        p++;
    }
    int i = 0;
    while (last[i] && last[i] != '/' && i < RETROFS_MAX_NAME - 1) {
        out[i] = last[i];
        i++;
    }
    out[i] = '\0';
}

static int retrofs_upsert_entry_in_directory(uint64_t dir_sector, const char* name,
                                             const struct retrofs_directory_entry_inner* new_entry) {
    uint64_t current = dir_sector;
    uint64_t last_sector = dir_sector;
    uint64_t empty_slot_sector = 0;
    size_t empty_slot_index = 0;
    int have_empty_slot = 0;

    while (current != 0) {
        union retrofs_directory_entry* block = 0;
        union retrofs_directory_entry* entries;
        uint64_t continuation = 0;
        uint64_t pages = 0;
        size_t entry_count;
        if (retrofs_load_directory(current, &block, &entry_count, &continuation, &pages) < 0) return -1;
        entries = &block[1];
        for (size_t i = 0; i < entry_count; i++) {
            struct retrofs_directory_entry_inner* entry = &entries[i].entry;
            if (entry->filename[0] == '\0') {
                // cprintf("upsert: found empty slot at index %d\r\n", (int)i);
                if (!have_empty_slot) {
                    have_empty_slot = 1;
                    empty_slot_sector = current;
                    empty_slot_index = i;
                }
                continue; /* keep scanning for a name match after deleted slots */
            }
            // cprintf("upsert: checking '%s' == '%s'?\r\n", entry->filename, name);
            if (strcmp_exact_retrofs(entry->filename, name)) {
                *entry = *new_entry;
                retrofs_write_sectors(current, block, (size_t)block[0].start.sectors);
                pmm_free((void*)VIRT_TO_PHYS((uint64_t)block), (int)pages);
                return 0;
            }
        }
        last_sector = current;
        current = continuation;
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)block), (int)pages);
    }

    if (have_empty_slot) {
        union retrofs_directory_entry* block = 0;
        uint64_t continuation = 0;
        uint64_t pages = 0;
        size_t entry_count;
        if (retrofs_load_directory(empty_slot_sector, &block, &entry_count, &continuation, &pages) < 0) return -1;
        block[1 + empty_slot_index].entry = *new_entry;
        retrofs_write_sectors(empty_slot_sector, block, (size_t)block[0].start.sectors);
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)block), (int)pages);
        return 0;
    }

    // Directory full: extend with new block
    int64_t new_block_sector = retrofs_alloc_sectors(RETROFS_DEFAULT_DIR_SECTORS);
    if (new_block_sector < 0) return -1;

    // Patch last block's continuation
    union retrofs_directory_entry* block = 0;
    uint64_t continuation = 0;
    uint64_t pages = 0;
    size_t entry_count;
    if (retrofs_load_directory(last_sector, &block, &entry_count, &continuation, &pages) < 0) return -1;
    block[0].start.continuation = (uint64_t)new_block_sector;
    retrofs_write_sectors(last_sector, block, (size_t)block[0].start.sectors);
    pmm_free((void*)VIRT_TO_PHYS((uint64_t)block), (int)pages);

    // Initialize new directory block
    void* phys = pmm_alloc((int)((RETROFS_DEFAULT_DIR_SECTORS * RETROFS_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE));
    union retrofs_directory_entry* newblk = (union retrofs_directory_entry*)PHYS_TO_VIRT(phys);
    memset(newblk, 0, RETROFS_DEFAULT_DIR_SECTORS * RETROFS_SECTOR_SIZE);
    newblk[0].start.flags = RETROFS_FLAG_DIR_START;
    strncpy_retrofs(newblk[0].start.title, "continued", RETROFS_MAX_NAME);
    newblk[0].start.parent = dir_sector;
    newblk[0].start.sectors = RETROFS_DEFAULT_DIR_SECTORS;
    newblk[1].entry = *new_entry;
    retrofs_write_sectors((uint64_t)new_block_sector, newblk, RETROFS_DEFAULT_DIR_SECTORS);
    pmm_free(phys, (int)((RETROFS_DEFAULT_DIR_SECTORS * RETROFS_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE));

    return 0;
}

int retrofs_write_file(const char* path, uint64_t offset, const void* buf, size_t size) {
    struct retrofs_directory_entry_inner entry;
    int is_root = 0;
    uint64_t parent_sector = 0;
    uint64_t start_lba;
    uint64_t sector_offset;
    uint64_t available;
    uint8_t sector[RETROFS_SECTOR_SIZE];
    const uint8_t* in = (const uint8_t*)buf;

    if (!g_retrofs.mounted || !buf) return -1;
    if (retrofs_walk_path(path, &entry, &is_root, &parent_sector) < 0) return -1;
    if (is_root || (entry.flags & RETROFS_FLAG_DIRECTORY)) return -1;

    if (offset + size > entry.sector_length * RETROFS_SECTOR_SIZE) {
        // Simple reallocation: allocate new extent and copy old data
        uint64_t new_sectors = (offset + size + RETROFS_SECTOR_SIZE - 1) / RETROFS_SECTOR_SIZE;
        // Basic reservation (at least 128KB for new files/extensions)
        if (new_sectors < 256) new_sectors = 256;
        int64_t new_start = retrofs_alloc_sectors(new_sectors);
        if (new_start < 0) return -1;

        if (entry.length > 0) {
            size_t old_pages = (size_t)((entry.sector_length * RETROFS_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
            if (old_pages == 0) old_pages = 1;
            void* old_phys = pmm_alloc((int)old_pages);
            if (old_phys) {
                void* old_virt = PHYS_TO_VIRT(old_phys);
                retrofs_read_sectors(entry.sector_start, old_virt, (size_t)entry.sector_length);
                retrofs_write_sectors((uint64_t)new_start, old_virt, (size_t)entry.sector_length);
                pmm_free(old_phys, (int)old_pages);
            }
            // Free old sectors
            for (uint64_t i = 0; i < entry.sector_length; i++) {
                retrofs_bitmap_set(entry.sector_start + i, 0);
            }
        }

        entry.sector_start = (uint64_t)new_start;
        entry.sector_length = new_sectors;
    }

    available = size;
    start_lba = entry.sector_start + (offset / RETROFS_SECTOR_SIZE);
    sector_offset = offset % RETROFS_SECTOR_SIZE;

    if (sector_offset) {
        uint64_t chunk = RETROFS_SECTOR_SIZE - sector_offset;
        if (chunk > available) chunk = available;
        if (retrofs_read_sectors(start_lba, sector, 1) < 0) return -1;
        for (uint64_t i = 0; i < chunk; i++) sector[sector_offset + i] = in[i];
        if (retrofs_write_sectors(start_lba, sector, 1) < 0) return -1;
        in += chunk;
        available -= chunk;
        start_lba++;
    }

    while (available >= RETROFS_SECTOR_SIZE) {
        if (retrofs_write_sectors(start_lba, in, 1) < 0) return -1;
        in += RETROFS_SECTOR_SIZE;
        available -= RETROFS_SECTOR_SIZE;
        start_lba++;
    }

    if (available) {
        if (retrofs_read_sectors(start_lba, sector, 1) < 0) return -1;
        for (uint64_t i = 0; i < available; i++) sector[i] = in[i];
        if (retrofs_write_sectors(start_lba, sector, 1) < 0) return -1;
    }

    if (offset + size > entry.length) {
        entry.length = offset + size;
    }
    
    char filename[RETROFS_MAX_NAME];
    get_filename_from_path(path, filename);
    return retrofs_upsert_entry_in_directory(parent_sector, filename, &entry);
}

int retrofs_read_file(const char* path, uint64_t offset, void* buf, size_t size) {
    struct retrofs_directory_entry_inner entry;
    int is_root = 0;
    uint64_t available;
    uint64_t start_lba;
    uint64_t sector_offset;
    uint64_t tail;
    uint8_t sector[RETROFS_SECTOR_SIZE];
    uint8_t* out = (uint8_t*)buf;

    if (!g_retrofs.mounted || !buf) return -1;
    if (retrofs_walk_path(path, &entry, &is_root, 0) < 0) return -1;
    if (is_root || (entry.flags & RETROFS_FLAG_DIRECTORY)) return -1;
    if (offset > entry.length) return -1;
    if (offset + size > entry.length) return -1;
    if (size == 0) return 0;

    available = size;
    start_lba = entry.sector_start + (offset / RETROFS_SECTOR_SIZE);
    sector_offset = offset % RETROFS_SECTOR_SIZE;

    if (sector_offset) {
        uint64_t chunk = RETROFS_SECTOR_SIZE - sector_offset;
        if (chunk > available) chunk = available;
        if (retrofs_read_sectors(start_lba, sector, 1) < 0) return -1;
        for (uint64_t i = 0; i < chunk; i++) out[i] = sector[sector_offset + i];
        out += chunk;
        available -= chunk;
        start_lba++;
    }

    while (available >= RETROFS_SECTOR_SIZE) {
        if (retrofs_read_sectors(start_lba, out, 1) < 0) return -1;
        out += RETROFS_SECTOR_SIZE;
        available -= RETROFS_SECTOR_SIZE;
        start_lba++;
    }

    tail = available;
    if (tail) {
        if (retrofs_read_sectors(start_lba, sector, 1) < 0) return -1;
        for (uint64_t i = 0; i < tail; i++) out[i] = sector[i];
    }
    return 0;
}

void retrofs_init(void) {
    memset(&g_retrofs, 0, sizeof(g_retrofs));
}

int retrofs_mount_storage(const char* device_name) {
    struct storage_device* dev;
    union retrofs_description_block_padded block;
    retrofs_init();
    if (!device_name) return -1;
    dev = storage_find_device(device_name);
    if (!dev || dev->block_size != RETROFS_SECTOR_SIZE) return -1;
    if (storage_read_blocks(device_name, 0, &block, 1) < 0) return -1;
    if (block.desc.identifier != RETROFS_ID) return -1;
    g_retrofs.device_name = dev->name;
    g_retrofs.start_lba = 0;
    g_retrofs.total_sectors = dev->block_count;
    g_retrofs.desc = block.desc;
    g_retrofs.mounted = 1;
    return 0;
}

int retrofs_is_mounted(void) {
    return g_retrofs.mounted;
}

const struct retrofs_context* retrofs_get_context(void) {
    return g_retrofs.mounted ? &g_retrofs : 0;
}

int retrofs_stat_path(const char* path, uint32_t* mode, uint64_t* size, int64_t* mtime) {
    struct retrofs_directory_entry_inner entry;
    int is_root = 0;
    if (!g_retrofs.mounted) return -1;
    if (retrofs_walk_path(path, &entry, &is_root, 0) < 0) return -1;
    if (is_root) {
        if (mode) *mode = KSTAT_MODE_DIR | 0555U;
        if (size) *size = 0;
        if (mtime) *mtime = g_retrofs.desc.creation_time;
        return 0;
    }
    if (mode) *mode = retrofs_mode_from_flags(entry.flags);
    if (size) *size = entry.length;
    if (mtime) *mtime = entry.modified;
    return 0;
}

int retrofs_list_dir(const char* path, struct orth_dirent* dirents, size_t max_entries, size_t* out_count) {
    struct retrofs_directory_entry_inner entry;
    uint64_t dir_sector = g_retrofs.desc.root_directory;
    uint64_t current;
    size_t count;
    int is_root = 0;
    if (!g_retrofs.mounted || !dirents || !out_count) return -1;
    count = *out_count;
    if (retrofs_walk_path(path, &entry, &is_root, 0) < 0) return -1;
    if (!is_root) {
        if ((entry.flags & RETROFS_FLAG_DIRECTORY) == 0) return -1;
        dir_sector = entry.sector_start;
    }
    current = dir_sector;
    while (current != 0) {
        union retrofs_directory_entry* block = 0;
        union retrofs_directory_entry* entries;
        uint64_t continuation = 0;
        uint64_t pages = 0;
        size_t entry_count;
        if (retrofs_load_directory(current, &block, &entry_count, &continuation, &pages) < 0) return -1;
        entries = &block[1];
        for (size_t i = 0; i < entry_count; i++) {
            struct retrofs_directory_entry_inner* e = &entries[i].entry;
            int j = 0;
            if (e->filename[0] == '\0') continue; /* skip deleted/unused slots */
            if (retrofs_dirent_name_exists(dirents, count, e->filename)) continue;
            if (count >= max_entries) {
                pmm_free((void*)VIRT_TO_PHYS((uint64_t)block), (int)pages);
                return -1;
            }
            dirents[count].mode = retrofs_mode_from_flags(e->flags);
            dirents[count].size = (uint32_t)e->length;
            for (; j + 1 < (int)sizeof(dirents[count].name) && e->filename[j]; j++) {
                dirents[count].name[j] = e->filename[j];
            }
            dirents[count].name[j] = '\0';
            count++;
        }
        current = continuation;
        pmm_free((void*)VIRT_TO_PHYS((uint64_t)block), (int)pages);
    }
    *out_count = count;
    return 0;
}

int retrofs_create_file(const char* path, uint32_t mode) {
    extern void puts(const char* s);
    struct retrofs_directory_entry_inner entry;
    int is_root = 0;
    uint64_t parent_sector = 0;
    char filename[RETROFS_MAX_NAME];
    (void)mode;

    if (!g_retrofs.mounted) return -1;
    if (retrofs_walk_path(path, &entry, &is_root, &parent_sector) == 0) {
        puts("retrofs_create: already exists\r\n");
        return -1; // Already exists
    }

    // Default reservation: 128KB (256 sectors)
    uint64_t reserve_sectors = 256;
    int64_t start_sector = retrofs_alloc_sectors(reserve_sectors);
    if (start_sector < 0) {
        puts("retrofs_create: alloc failed\r\n");
        return -1;
    }

    memset(&entry, 0, sizeof(entry));
    get_filename_from_path(path, filename);
    strncpy_retrofs(entry.filename, filename, RETROFS_MAX_NAME);
    entry.flags = 0;
    entry.sector_start = (uint64_t)start_sector;
    entry.length = 0;
    entry.sector_length = reserve_sectors;
    entry.created = g_retrofs.desc.creation_time;
    entry.modified = entry.created;
    entry.sequence = 1;

    return retrofs_upsert_entry_in_directory(parent_sector, filename, &entry);
}

int retrofs_truncate_file(const char* path, uint64_t size) {
    struct retrofs_directory_entry_inner entry;
    int is_root = 0;
    uint64_t parent_sector = 0;
    if (!g_retrofs.mounted) return -1;
    if (retrofs_walk_path(path, &entry, &is_root, &parent_sector) < 0) return -1;
    if (is_root || (entry.flags & RETROFS_FLAG_DIRECTORY)) return -1;

    entry.length = size;
    char filename[RETROFS_MAX_NAME];
    get_filename_from_path(path, filename);
    return retrofs_upsert_entry_in_directory(parent_sector, filename, &entry);
}

int retrofs_unlink(const char* path) {
    struct retrofs_directory_entry_inner entry;
    int is_root = 0;
    uint64_t parent_sector = 0;
    if (!g_retrofs.mounted) return -1;
    if (retrofs_walk_path(path, &entry, &is_root, &parent_sector) < 0) return -1;
    if (is_root || (entry.flags & RETROFS_FLAG_DIRECTORY)) return -1;

    // Free sectors
    for (uint64_t i = 0; i < entry.sector_length; i++) {
        retrofs_bitmap_set(entry.sector_start + i, 0);
    }

    // Mark entry as empty
    memset(&entry, 0, sizeof(entry));
    char filename[RETROFS_MAX_NAME];
    get_filename_from_path(path, filename);
    return retrofs_upsert_entry_in_directory(parent_sector, filename, &entry);
}

int retrofs_rmdir(const char* path) {
    extern void puts(const char* s);
    struct retrofs_directory_entry_inner entry;
    int is_root = 0;
    uint64_t parent_sector = 0;
    if (!g_retrofs.mounted) {
        puts("rmdir: not mounted\r\n");
        return -1;
    }
    if (retrofs_walk_path(path, &entry, &is_root, &parent_sector) < 0) {
        return -1;
    }
    if (is_root || !(entry.flags & RETROFS_FLAG_DIRECTORY)) {
        puts("rmdir: is_root or not dir\r\n");
        return -1;
    }

    // Free sectors
    for (uint64_t i = 0; i < entry.sector_length; i++) {
        retrofs_bitmap_set(entry.sector_start + i, 0);
    }

    // Mark entry as empty
    memset(&entry, 0, sizeof(entry));
    char filename[RETROFS_MAX_NAME];
    get_filename_from_path(path, filename);
    int rc = retrofs_upsert_entry_in_directory(parent_sector, filename, &entry);
    if (rc < 0) puts("rmdir: upsert failed\r\n");
    return rc;
}

int retrofs_mkdir(const char* path, uint32_t mode) {
    extern void puts(const char* s);
    struct retrofs_directory_entry_inner entry;
    int is_root = 0;
    uint64_t parent_sector = 0;
    char filename[RETROFS_MAX_NAME];
    (void)mode;

    if (!g_retrofs.mounted) return -1;
    if (retrofs_walk_path(path, &entry, &is_root, &parent_sector) == 0) {
        puts("retrofs_mkdir: already exists\r\n");
        return -1;
    }

    // Allocate directory block
    int64_t start_sector = retrofs_alloc_sectors(RETROFS_DEFAULT_DIR_SECTORS);
    if (start_sector < 0) {
        puts("retrofs_mkdir: alloc failed\r\n");
        return -1;
    }

    // Initialize new directory block
    void* phys = pmm_alloc((int)((RETROFS_DEFAULT_DIR_SECTORS * RETROFS_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE));
    union retrofs_directory_entry* newblk = (union retrofs_directory_entry*)PHYS_TO_VIRT(phys);
    memset(newblk, 0, RETROFS_DEFAULT_DIR_SECTORS * RETROFS_SECTOR_SIZE);
    newblk[0].start.flags = RETROFS_FLAG_DIR_START | RETROFS_FLAG_DIRECTORY;
    get_filename_from_path(path, filename);
    strncpy_retrofs(newblk[0].start.title, filename, RETROFS_MAX_NAME);
    newblk[0].start.parent = parent_sector;
    newblk[0].start.sectors = RETROFS_DEFAULT_DIR_SECTORS;
    retrofs_write_sectors((uint64_t)start_sector, newblk, RETROFS_DEFAULT_DIR_SECTORS);
    pmm_free(phys, (int)((RETROFS_DEFAULT_DIR_SECTORS * RETROFS_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE));

    // Upsert entry in parent
    memset(&entry, 0, sizeof(entry));
    strncpy_retrofs(entry.filename, filename, RETROFS_MAX_NAME);
    entry.flags = RETROFS_FLAG_DIRECTORY;
    entry.sector_start = (uint64_t)start_sector;
    entry.length = 0;
    entry.sector_length = RETROFS_DEFAULT_DIR_SECTORS;
    entry.created = g_retrofs.desc.creation_time;
    entry.modified = entry.created;

    return retrofs_upsert_entry_in_directory(parent_sector, filename, &entry);
}

int retrofs_sync(void) {
    if (!g_retrofs.mounted) return -1;
    g_retrofs.desc.sequence++;
    union retrofs_description_block_padded block;
    memset(&block, 0, sizeof(block));
    block.desc = g_retrofs.desc;
    return retrofs_write_sectors(0, &block, 1);
}
