#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

// COW を誘発するための大きな配列
char shared_data[8192] = "Initial Data";

int main() {
    printf("COW Test Start. Initial data: %s\n", shared_data);

    pid_t pid = fork();
    if (pid == 0) {
        // 子プロセス
        printf("Child: Writing to shared_data...\n");
        shared_data[0] = 'C';
        printf("Child: shared_data: %s\n", shared_data);
        _exit(0);
    } else if (pid > 0) {
        // 親プロセス
        int status;
        waitpid(pid, &status, 0);
        printf("Parent: Child finished. Parent writing to shared_data...\n");
        shared_data[0] = 'P';
        printf("Parent: shared_data: %s\n", shared_data);
    } else {
        perror("fork");
        return 1;
    }

    return 0;
}
