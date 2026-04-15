#ifndef RETROFS_H
#define RETROFS_H

#include "fs.h"
#include <stddef.h>
#include <stdint.h>

#define RETROFS_SECTOR_SIZE 512U
#define RETROFS_MAX_NAME 128
#define RETROFS_DEFAULT_DIR_SECTORS 64U
#define RETROFS_MAX_DIR_SECTORS 256U
#define RETROFS_FLAG_DIRECTORY 0x0001U
#define RETROFS_FLAG_LOCKED    0x0002U
#define RETROFS_FLAG_DIR_START 0x0004U
#define RETROFS_ID 0x3153466f72746552ULL

struct retrofs_description_block {
    uint64_t identifier;
    uint64_t root_directory;
    uint64_t free_space_map_start;
    uint64_t free_space_map_length;
    uint64_t free_space_map_checksum;
    uint64_t sequence;
    int64_t creation_time;
} __attribute__((packed));

union retrofs_description_block_padded {
    struct retrofs_description_block desc;
    char raw[RETROFS_SECTOR_SIZE];
} __attribute__((packed));

struct retrofs_directory_start {
    uint32_t flags;
    char title[RETROFS_MAX_NAME];
    uint64_t parent;
    uint64_t sectors;
    uint64_t continuation;
    char reserved[];
} __attribute__((packed));

struct retrofs_directory_entry_inner {
    uint32_t flags;
    char filename[RETROFS_MAX_NAME];
    uint64_t sector_start;
    uint64_t length;
    uint64_t sector_length;
    int64_t created;
    int64_t modified;
    uint64_t sequence;
    char reserved[];
} __attribute__((packed));

union retrofs_directory_entry {
    struct retrofs_directory_start start;
    struct retrofs_directory_entry_inner entry;
    char raw[RETROFS_SECTOR_SIZE / 2];
} __attribute__((packed));

struct retrofs_context {
    const char* device_name;
    uint64_t start_lba;
    uint64_t total_sectors;
    struct retrofs_description_block desc;
    int mounted;
};

void retrofs_init(void);
int retrofs_mount_storage(const char* device_name);
int retrofs_is_mounted(void);
const struct retrofs_context* retrofs_get_context(void);
int retrofs_stat_path(const char* path, uint32_t* mode, uint64_t* size, int64_t* mtime);
int retrofs_read_file(const char* path, uint64_t offset, void* buf, size_t size);
int retrofs_write_file(const char* path, uint64_t offset, const void* buf, size_t size);
int retrofs_create_file(const char* path, uint32_t mode);
int retrofs_truncate_file(const char* path, uint64_t size);
int retrofs_mkdir(const char* path, uint32_t mode);
int retrofs_unlink(const char* path);
int retrofs_rmdir(const char* path);
int retrofs_list_dir(const char* path, struct orth_dirent* dirents, size_t max_entries, size_t* out_count);
int retrofs_sync(void);

#endif
