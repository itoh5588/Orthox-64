#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

#define MAX_FDS 32

// USTAR TAR ヘッダ構造
struct tar_header {
    char name[100];     // ファイル名
    char mode[8];       // モード
    char uid[8];        // ユーザーID
    char gid[8];        // グループID
    char size[12];      // サイズ (8進数文字列)
    char mtime[12];     // 更新日時
    char chksum[8];     // チェックサム
    char typeflag;      // タイプ (0: 通常ファイル, 5: ディレクトリ)
    char linkname[100];
    char magic[6];      // "ustar"
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
};

typedef enum {
    FT_UNUSED,
    FT_CONSOLE, // stdin/stdout/stderr
    FT_MODULE,  // Limine 直接ロード
    FT_TAR,     // TAR内ファイル
    FT_RAMFS,   // メモリ上の書き込み可能ファイル
    FT_PIPE,    // パイプ
    FT_SOCKET,  // lwIP-backed socket
    FT_USB,     // USB FAT file
    FT_USBROOT, // file inside mounted USB TAR root
    FT_DIR      // synthesized directory listing
} file_type_t;

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_APPEND    0x0008
#define O_CREAT     0x0200
#define O_TRUNC     0x0400
#define O_DIRECTORY 0x10000

#define PIPE_BUF_SIZE 4000

typedef struct {
    char buffer[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    int ref_count;
} pipe_t;

typedef struct {
    file_type_t type;
    void* data;      // ファイルデータへのポインタ (FT_RAMFS の場合は malloc 領域, FT_PIPE の場合は pipe_t)
    size_t size;     // ファイルサイズ
    size_t offset;   // 現在の読み取り/書き込みオフセット
    int in_use;      // 使用中フラグ
    int flags;       // O_RDONLY, O_WRONLY, O_RDWR
    int fd_flags;    // FD_CLOEXEC などの descriptor flags
    char name[64];   // ファイル名 (Ramfs用)
    uint32_t aux0;   // backend-specific metadata
    uint32_t aux1;   // backend-specific metadata
} file_descriptor_t;

#define KSTAT_MODE_FILE 0100000
#define KSTAT_MODE_DIR  0040000
#define KSTAT_MODE_CHR  0020000

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

#ifndef ORTH_DIRENT_DEFINED
#define ORTH_DIRENT_DEFINED
struct orth_dirent {
    uint32_t mode;
    uint32_t size;
    char name[248];
};
#endif

void fs_init(void);
int sys_open(const char* path, int flags, int mode);
int sys_openat(int dirfd, const char* path, int flags, int mode);
int64_t sys_read(int fd, void* buf, size_t count);
int sys_close(int fd);
int sys_fstat(int fd, struct kstat* st);
int sys_stat(const char* path, struct kstat* st);
int sys_fstatat(int dirfd, const char* path, struct kstat* st, int flags);
int64_t sys_lseek(int fd, int64_t offset, int whence);
int sys_unlink(const char* path);
int sys_unlinkat(int dirfd, const char* path, int flags);
int sys_rename(const char* oldpath, const char* newpath);
int sys_chmod(const char* path, uint32_t mode);
int sys_chdir(const char* path);
int sys_fchdir(int fd);
int sys_mkdir(const char* path, int mode);
int sys_mkdirat(int dirfd, const char* path, int mode);
int sys_rmdir(const char* path);
int sys_getcwd(char* buf, size_t size);
int sys_getdents(int fd, struct orth_dirent* dirp, size_t count);
int sys_getdents64(int fd, void* dirp, size_t count);
int sys_fcntl(int fd, int cmd, uint64_t arg);
int sys_pipe2(int pipefd[2], int flags);
int fs_mount_usb_root_tar(const char* path);
int fs_mount_module_root(void);
int fs_get_mount_status(char* buf, size_t size);

#endif
