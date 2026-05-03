#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

void sync(void);

int main() {
    printf("retrofsrw:start\n");

    // 1. Create a directory
    if (mkdir("/testdir", 0777) < 0) {
        perror("mkdir /testdir failed");
    } else {
        printf("retrofsrw:mkdir_done\n");
    }

    // 2. Create and write to a file
    int fd = open("/testdir/test_rw.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open failed");
        return 1;
    }

    const char* msg = "Hello_RetroFS_RW_Test\n";
    if (write(fd, msg, strlen(msg)) != (ssize_t)strlen(msg)) {
        perror("write failed");
        return 1;
    }
    close(fd);

    printf("retrofsrw:write_done\n");

    // 3. sync
    sync();

    printf("retrofsrw:sync_done\n");

    // We can exit here. We'll verify persistence on the host by searching for the string in rootfs.img.
    
    // Actually we also need to test unlink and rmdir. 
    // Maybe we create another file and delete it.
    fd = open("/testdir/test_del.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "to_be_deleted", 13);
        close(fd);
    }
    if (unlink("/testdir/test_del.txt") < 0) {
        perror("unlink failed");
        return 1;
    }

    printf("retrofsrw:unlink_done\n");

    // Directory removal test
    if (mkdir("/testdir2", 0777) == 0) {
        if (rmdir("/testdir2") < 0) {
            perror("rmdir failed");
            return 1;
        }
        printf("retrofsrw:rmdir_done\n");
    }

    printf("retrofsrw:end\n");
    return 0;
}
