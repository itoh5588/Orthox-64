#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int check_dir_has(const char *path, const char *want_a, const char *want_b) {
    DIR *dir;
    struct dirent *ent;
    int count = 0;
    int found_a = 0;
    int found_b = 0;

    dir = opendir(path);
    if (!dir) {
        perror(path);
        return -1;
    }

    while ((ent = readdir(dir)) != 0) {
        count++;
        if (want_a && strcmp(ent->d_name, want_a) == 0) found_a = 1;
        if (want_b && strcmp(ent->d_name, want_b) == 0) found_b = 1;
    }
    closedir(dir);

    printf("readdir %s count=%d found_%s=%d found_%s=%d\n",
           path,
           count,
           want_a ? want_a : "-",
           found_a,
           want_b ? want_b : "-",
           found_b);
    return (!want_a || found_a) && (!want_b || found_b) ? 0 : -1;
}

int main(void) {
    struct stat st;
    int fd;
    unsigned char hdr[4];
    char prefix[64];
    int n;

    if (stat("/", &st) < 0) {
        perror("stat /");
        return 1;
    }
    printf("stat / ok mode=%o size=%ld\n", st.st_mode, (long)st.st_size);

    if (check_dir_has("/", "bin", "lib") < 0) return 2;

    if (stat("/bin", &st) < 0) {
        perror("stat /bin");
        return 3;
    }
    printf("stat /bin ok mode=%o size=%ld\n", st.st_mode, (long)st.st_size);

    if (check_dir_has("/bin", "python3", "sh") < 0) return 4;

    if (stat("/bin/python3", &st) < 0) {
        perror("stat /bin/python3");
        return 5;
    }
    printf("stat /bin/python3 ok size=%ld\n", (long)st.st_size);

    fd = open("/bin/python3", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open /bin/python3");
        return 6;
    }
    n = (int)read(fd, hdr, sizeof(hdr));
    close(fd);
    if (n != (int)sizeof(hdr)) {
        printf("read /bin/python3 short n=%d\n", n);
        return 7;
    }
    printf("read /bin/python3 hdr=%02x %02x %02x %02x\n", hdr[0], hdr[1], hdr[2], hdr[3]);
    if (hdr[0] != 0x7f || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F') {
        return 8;
    }

    if (stat("/lib/python312.zip", &st) < 0) {
        perror("stat /lib/python312.zip");
        return 9;
    }
    printf("stat /lib/python312.zip ok size=%ld\n", (long)st.st_size);

    if (stat("/lib/python3.12/encodings/__init__.py", &st) < 0) {
        perror("stat /lib/python3.12/encodings/__init__.py");
        return 10;
    }
    printf("stat /lib/python3.12/encodings/__init__.py ok size=%ld\n", (long)st.st_size);

    fd = open("/lib/python3.12/encodings/__init__.py", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open /lib/python3.12/encodings/__init__.py");
        return 11;
    }
    n = (int)read(fd, prefix, sizeof(prefix) - 1);
    close(fd);
    if (n < 0) {
        perror("read /lib/python3.12/encodings/__init__.py");
        return 12;
    }
    prefix[n] = '\0';
    printf("read encodings prefix=%.40s\n", prefix);
    return 0;
}
