/*
 * fs.c - File system initialization (VFS + RamFS + embedded files)
 *
 * Separated from sched.c to respect single responsibility:
 *   - sched.c: process/task management only
 *   - fs.c:    file system initialization and mounting
 */

#include "include/log.h"
#include "vfs.h"
#include "fs.h"
#include "pagetable.h"

/* ================================================================
 * fs_init — Initialize VFS, RamFS, embedded files, and launch init
 * ================================================================ */
void fs_init(void) {
    vfs_init();

    struct super_block *ram = ramfs_create();
    if (ram) {
        ramfs_add_file("hello.txt", "This is a ramfs file.\n");
        vfs_mount_root(ram);
        log_printf(LOG_LEVEL_INFO, "fs: RamFS mounted\n");

        embed_init();

        if (vfs_lookup("/hello")) {
            log_printf(LOG_LEVEL_INFO, "fs: Found /hello in ramfs, attempting exec\n");
            int pid = exec_elf("/hello");
            log_printf(LOG_LEVEL_INFO, "fs: exec_elf returned pid=%d\n", pid);
        }
    }
}