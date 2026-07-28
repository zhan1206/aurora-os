/*
 * devtmpfs.h - devtmpfs device filesystem header
 *
 * Provides a simple /dev virtual filesystem that auto-creates
 * device nodes. Inspired by CoolPotOS's devtmpfs.
 *
 * Supported devices:
 *   /dev/null    - Data sink (write) / EOF (read)
 *   /dev/zero    - Zero bytes source (read)
 *   /dev/console - System console
 *   /dev/tty     - Current terminal
 *   /dev/random  - Hardware random number generator (RDRAND, blocking)
 *   /dev/urandom - Hardware random number generator (RDRAND, non-blocking)
 *   /dev/stdin   - Standard input (fd 0 mapping)    (v4.2.6)
 *   /dev/stdout  - Standard output (fd 1 mapping)   (v4.2.6)
 *   /dev/stderr  - Standard error (fd 2 mapping)    (v4.2.6)
 */

#ifndef DEVTMPFS_H
#define DEVTMPFS_H

struct super_block;

/* Initialize and mount devtmpfs at /dev */
void devtmpfs_init(void);

/* FIXED (v4.3.2): USB-001 — Create /dev/usb directory and device nodes */
void devtmpfs_create_usb_nodes(void);

/* Create the devtmpfs super block */
struct super_block *devtmpfs_create(void);

#endif /* DEVTMPFS_H */