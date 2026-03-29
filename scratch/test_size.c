#include <stdio.h>
#include <sys/stat.h>

int main() {
    struct stat st;
    printf("st_mode offset: %zu\n", (size_t)&((struct stat *)0)->st_mode);
    printf("st_size offset: %zu\n", (size_t)&((struct stat *)0)->st_size);
    printf("total size: %zu\n", sizeof(struct stat));
    return 0;
}
