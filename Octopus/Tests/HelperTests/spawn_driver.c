/* Runs oct_spawn for the host tests: spawn_driver [--root] <path> [args...]. Prints "CODE <n>", "LEN <bytes>", then
 * exactly what the child printed. */
#include "../../App/Spawn/oct_spawn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    int first = 1, as_root = 0;
    if (argc > 1 && strcmp(argv[1], "--root") == 0) {
        as_root = 1;
        first = 2;
    }
    if (first >= argc) return 64;
    char *output = NULL;
    int code = oct_spawn(argv[first], (const char *const *)(argv + first), as_root, &output);
    size_t length = output ? strlen(output) : 0;
    printf("CODE %d\nLEN %zu\n", code, length);
    if (output) fwrite(output, 1, length, stdout);
    free(output);
    return 0;
}
