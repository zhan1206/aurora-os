/* FIXED (v4.3.8): USER-001 — User-mode ls program
 * Lists files in the current directory using getdents syscall. */
#include "../userspace/libc.c"

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* Use getdents syscall (SYS_GETDENTS64 = 217) */
    char buf[4096];
    long n = sys_call(217, 0, (long)buf, (long)sizeof(buf));

    if (n <= 0) {
        puts("ls: no entries or error");
        return 1;
    }

    /* Parse linux_dirent64 entries */
    char *ptr = buf;
    long remaining = n;
    while (remaining > 0) {
        /* d_ino (8 bytes) + d_off (8 bytes) + d_reclen (2 bytes) + d_type (1 byte) + d_name */
        unsigned short reclen = *(unsigned short *)(ptr + 16);
        if (reclen == 0 || reclen > (unsigned short)remaining) break;

        char *name = ptr + 19;  /* Skip d_ino(8) + d_off(8) + d_reclen(2) + d_type(1) */
        unsigned char type = *(unsigned char *)(ptr + 18);

        if (name[0] == '\0') break;

        /* Print name with type indicator */
        write(1, name, strlen(name));
        if (type == 4) write(1, "/", 1);  /* DT_DIR */
        write(1, "  ", 2);

        ptr += reclen;
        remaining -= reclen;
    }
    write(1, "\n", 1);
    return 0;
}