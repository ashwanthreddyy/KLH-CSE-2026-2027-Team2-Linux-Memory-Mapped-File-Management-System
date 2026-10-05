#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>

int main() {
    int fd;
    struct stat st;
    char *data;

    fd = open("mmap.txt", O_RDWR);

    if (fd == -1) {
        perror("open");
        return 1;
    }

    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        return 1;
    }

    data = mmap(NULL,
                st.st_size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd,
                0);

    if (data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    printf("File contents:\n");
    write(STDOUT_FILENO, data, st.st_size);

    printf("\n\nModifying file using mmap...\n");

    if (st.st_size >= 5) {
        memcpy(data, "HELLO", 5);
    }

    msync(data, st.st_size, MS_SYNC);

    munmap(data, st.st_size);
    close(fd);

    printf("File updated successfully.\n");

    return 0;
}
