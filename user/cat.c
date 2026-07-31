/* FIXED (v4.3.8): USER-002 — User-mode cat program
 * Reads and prints file contents to stdout. */
#include "../userspace/libc.c"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        puts("cat: missing file operand");
        puts("Usage: cat <file>");
        return 1;
    }

    /* Open the file */
    long fd = sys_call(2, (long)argv[1], 0, 0);  /* SYS_OPEN */
    if (fd < 0) {
        puts("cat: cannot open file");
        return 1;
    }

    /* Read and print in chunks */
    char buf[512];
    long n;
    while ((n = sys_call(0, fd, (long)buf, (long)sizeof(buf))) > 0) {
        write(1, buf, (unsigned long)n);
    }

    sys_call(3, fd, 0, 0);  /* SYS_CLOSE */
    return 0;
}