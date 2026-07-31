/* FIXED (v4.3.8): USER-003 — User-mode echo program
 * Prints arguments to stdout, separated by spaces. */
#include "../userspace/libc.c"

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        write(1, argv[i], strlen(argv[i]));
        if (i < argc - 1) write(1, " ", 1);
    }
    write(1, "\n", 1);
    return 0;
}