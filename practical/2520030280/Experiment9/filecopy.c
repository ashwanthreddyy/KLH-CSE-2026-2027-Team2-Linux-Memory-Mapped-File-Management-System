#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#define BUFFER_SIZE 4096

int main(int argc, char *argv[]) {
    int src, dest;
    char buffer[BUFFER_SIZE];
    ssize_t bytesRead, bytesWritten;

    if (argc != 3) {
        printf("Usage: %s <source> <destination>\n", argv[0]);
        return 1;
    }

    src = open(argv[1], O_RDONLY);

    if (src == -1) {
        perror("Error opening source file");
        return 1;
    }

    dest = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (dest == -1) {
        perror("Error opening destination file");
        close(src);
        return 1;
    }

    while ((bytesRead = read(src, buffer, BUFFER_SIZE)) > 0) {
        bytesWritten = write(dest, buffer, bytesRead);

        if (bytesWritten != bytesRead) {
            perror("Error writing file");
            close(src);
            close(dest);
            return 1;
        }
    }

    if (bytesRead == -1) {
        perror("Error reading file");
    }

    close(src);
    close(dest);

    printf("File copied successfully.\n");

    return 0;
}
