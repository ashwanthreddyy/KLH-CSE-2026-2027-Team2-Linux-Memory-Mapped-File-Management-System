#include <stdio.h>
#include <stdlib.h>

#define BUFFER_SIZE 4096

int main(int argc, char *argv[]) {
    FILE *src, *dest;
    char buffer[BUFFER_SIZE];
    size_t bytesRead;

    if (argc != 3) {
        printf("Usage: %s <source> <destination>\n", argv[0]);
        return 1;
    }

    src = fopen(argv[1], "rb");

    if (src == NULL) {
        perror("Error opening source file");
        return 1;
    }

    dest = fopen(argv[2], "wb");

    if (dest == NULL) {
        perror("Error opening destination file");
        fclose(src);
        return 1;
    }

    while ((bytesRead = fread(buffer, 1, BUFFER_SIZE, src)) > 0) {
        fwrite(buffer, 1, bytesRead, dest);
    }

    fclose(src);
    fclose(dest);

    printf("File copied successfully using standard I/O.\n");

    return 0;
}
