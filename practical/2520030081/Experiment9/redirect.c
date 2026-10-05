#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

int main() {
    int input, output;
    char buffer[100];

    input = open("input.txt", O_RDONLY);

    if (input == -1) {
        perror("Error opening input.txt");
        return 1;
    }

    output = open("output.txt",
                  O_WRONLY | O_CREAT | O_TRUNC,
                  0644);

    if (output == -1) {
        perror("Error opening output.txt");
        close(input);
        return 1;
    }

    dup2(input, STDIN_FILENO);
    dup2(output, STDOUT_FILENO);

    close(input);
    close(output);

    while (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        printf("Read: %s", buffer);
    }

    return 0;
}
