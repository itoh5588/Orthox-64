#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char *msg, int code) {
    printf("%s\n", msg);
    return code;
}

int main(void) {
    struct stat st;
    DIR *dir;
    int fd;
    char buf[32];
    int n;

    printf("retrofsedge: start\n");

    if (stat("/no-such-path", &st) == 0) return fail("stat missing unexpectedly succeeded", 1);
    if (open("/no-such-file", O_RDONLY) >= 0) return fail("open missing unexpectedly succeeded", 2);
    dir = opendir("/no-such-dir");
    if (dir) {
        closedir(dir);
        return fail("opendir missing unexpectedly succeeded", 3);
    }

    if (stat("/lib/python3.12", &st) < 0) return fail("stat /lib/python3.12 failed", 4);
    if ((st.st_mode & 0170000) != 0040000) return fail("/lib/python3.12 is not dir", 5);

    if (chdir("/lib/python3.12") < 0) return fail("chdir /lib/python3.12 failed", 6);
    if (stat("encodings/__init__.py", &st) < 0) return fail("relative stat encodings failed", 7);
    if ((st.st_mode & 0170000) != 0100000) return fail("encodings/__init__.py is not file", 8);

    fd = open("encodings/__init__.py", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail("relative open encodings failed", 9);
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return fail("relative read encodings failed", 10);
    buf[n] = '\0';
    printf("relative read prefix=%.24s\n", buf);

    if (chdir("/") < 0) return fail("chdir / failed", 11);
    dir = opendir("/lib/python3.12/encodings");
    if (!dir) return fail("opendir encodings failed", 12);
    closedir(dir);

    fd = open("/lib/python3.12", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        close(fd);
        return fail("open directory unexpectedly succeeded", 13);
    }

    printf("retrofsedge: PASS\n");
    return 0;
}
