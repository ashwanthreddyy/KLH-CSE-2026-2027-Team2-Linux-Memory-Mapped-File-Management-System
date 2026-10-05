#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define BUFFER_SIZE 4096

double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void copy_system(const char *source, const char *destination) {
    int src, dest;
    char buffer[BUFFER_SIZE];
    ssize_t n;

    src = open(source, O_RDONLY);
    dest = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (src < 0 || dest < 0) {
        perror("open");
        return;
    }

    while ((n = read(src, buffer, BUFFER_SIZE)) > 0) {
        write(dest, buffer, n);
    }

    close(src);
    close(dest);
}

void copy_stdio(const char *source, const char *destination) {
    FILE *src, *dest;
    char buffer[BUFFER_SIZE];
    size_t n;

    src = fopen(source, "rb");
    dest = fopen(destination, "wb");

    if (src == NULL || dest == NULL) {
        perror("fopen");
        return;
    }

    while ((n = fread(buffer, 1, BUFFER_SIZE, src)) > 0) {
        fwrite(buffer, 1, n, dest);
    }

    fclose(src);
    fclose(dest);
}

int main() {
    double start, end;

    start = get_time();

    copy_system("source.txt", "system_copy.txt");

    end = get_time();

    printf("System call I/O time: %.6f seconds\n", end - start);

    start = get_time();

    copy_stdio("source.txt", "stdio_copy.txt");

    end = get_time();

    printf("Standard I/O time: %.6f seconds\n", end - start);

    return 0;
}
