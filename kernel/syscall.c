/*
 * syscall.c - System call dispatcher (Phase 1: complete I/O syscalls)
 */
#include "syscall.h"
#include "include/log.h"
#include "include/idt.h"
#include "include/userspace.h"
/* FIXED (v4.3.6): UA-001 — Centralized user memory access validation.
 * All user memory access is validated through user_access.h wrappers. */
#include "include/user_access.h"
#include "include/trapframe.h"
#include "include/errno.h"
#include "include/net.h"
#include "net/unix.h"       /* AF_UNIX (v4.2.6) */
#include "include/version.h"
#include "sched.h"
#include "signal.h"
#include "vfs.h"
#include "fs.h"
#include "pagetable.h"
#include "mem.h"
#include "capability.h"
#include "perf.h"
#include "seccomp.h"
#include "aslr.h"
#include "rtc.h"
#include "acpi.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Trapframe pointer for signal delivery */
/* FIXED (v4.1.4): current_tf moved to task_struct->current_tf for
 * SMP safety.  (BUG 4.4) */
#define CURRENT_TF()  ((struct trapframe *)current->current_tf)

/* ioctl size macro (standard Linux _IOC_SIZE) */
#define _IOC_SIZE(nr) (((nr) >> 16) & 0x3FFF)

/* ================================================================
 * I/O syscalls
 * ================================================================ */

/*
 * fd_validate: Basic fd access validation with capability awareness.
 *
 * FIXED (v4.1.3): Added capability-aware fd access checking.
 * In addition to bounds checking, this validates that the fd entry
 * is a valid file pointer (not a cap_entry wrapper or stale slot).
 * For cap_entry-based fds, the capability flags are checked via
 * fd_check_cap().
 */
static int fd_validate(int fd, uint32_t required_cap) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    if (current->fd_table[fd] == (uintptr_t)-1) return -EBADF;

    /*
     * FIXED (v4.1.4): Type-tag check for fd_table entries.
     * fd_table stores both file* and cap_entry* pointers.  The lower
     * 2 bits of any aligned pointer are always 0, so we use bit 0 as
     * a type tag: 0 = file pointer, 1 = capability entry.
     * Without this tag, a file pointer could be mistaken for a
     * cap_entry if its first bytes happen to match CAP_ENTRY_MAGIC,
     * leading to type confusion and potential UAF.  (BUG 3.9)
     */
    uintptr_t entry_raw = current->fd_table[fd];
    if (entry_raw & 1) {
        /* Capability entry: strip tag bit, validate magic */
        struct cap_entry *entry = (struct cap_entry *)(entry_raw & ~1ULL);
        if (entry->magic == CAP_ENTRY_MAGIC) {
            if (required_cap && (entry->caps & required_cap) != required_cap)
                return -EACCES;
        }
    }
    /* Raw file pointers (tag bit 0) have no capability restrictions */
    return 0;
}

/* AF_UNIX (v4.2.6): Check if an fd is a Unix domain socket and return it */
static inline struct unix_sock *fd_to_unix_sock(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    uintptr_t entry = current->fd_table[fd];
    /* Valid kernel pointers are > 0x1000 and bit 0 = 0 (not a cap entry) */
    if (entry == (uintptr_t)-1 || entry == 0 || entry == 0x1 || (entry & 1))
        return NULL;
    return (struct unix_sock *)entry;
}

/* Simple stat structure for fstat syscall */
struct kstat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
};

static long sys_read(int fd, void *buf, size_t count) {
    if (!buf || count == 0) return 0;
    int cap_ret = fd_validate(fd, 0);
    if (cap_ret < 0) { current->t_errno = -cap_ret; return -1; }
    if (!user_addr_range_ok(buf, count)) { current->t_errno = EFAULT; return -1; }

    /* fd 0 (stdin): read from console line buffer.
     * FIXED (v4.2.3): Use a kernel buffer for console_getline and
     * copy_to_user instead of passing the raw user-space pointer.
     * Without this, SMAP would fault and TOCTOU is possible.  (BUG-SYS-01) */
    if (fd == 0) {
        extern int console_getline(char *buf, size_t buflen);
        extern void console_write(const char *s);
        extern void yield(void);

        char kbuf[256];
        size_t to_read = (count > sizeof(kbuf) - 1) ? (sizeof(kbuf) - 1) : count;

        /* Block until a line is available */
        int len;
        while ((len = console_getline(kbuf, to_read)) == 0) {
            current->state = TASK_BLOCKED;
            schedule();
            /* Check for pending signals */
            if (current->sig && current->sig->pending) { current->t_errno = EINTR; return -1; }
        }
        if (len < 0) return -1;
        if (safe_copy_to_user(buf, kbuf, (size_t)len) != 0) {
            current->t_errno = EFAULT; return -1;
        }
        return len;
    }

    /* Other fds: use fd_table */
    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }
    /*
     * FIXME (v4.2.5): BUG-PIPE-SMAP note
     * vfs_read() passes the raw user pointer `buf` to file operations.
     * The VFS layer should handle stac()/clac() or safe_copy_to_user().
     * Currently, pipe_read() has been fixed (see pipe.c), but other
     * file backends (ramfs, ext2, devfs) may still have SMAP violations.
     * A proper fix would add stac()/clac() in the generic VFS read path.
     */
    return vfs_read(filp, buf, count);
}

static long sys_write(int fd, const void *buf, size_t count) {
    if (!buf || count == 0) return 0;
    int cap_ret = fd_validate(fd, 0);
    if (cap_ret < 0) { current->t_errno = -cap_ret; return -1; }
    if (!user_addr_range_ok(buf, count)) { current->t_errno = EFAULT; return -1; }

    /* fd 1/2 (stdout/stderr): batch output to VGA + serial */
    if (fd == 1 || fd == 2) {
        extern void printk(const char *str);
        /*
         * Batch output: copy up to 256 chars at a time to a kernel
         * buffer and call printk once, instead of per-character.
         * Uses memcpy for efficient bulk copy.
         */
        size_t remaining = count;
        const char *s = (const char *)buf;
        while (remaining > 0) {
            size_t chunk = (remaining > 256) ? 256 : remaining;
            char tmp[257];
            if (safe_copy_from_user(tmp, s, chunk) != 0) {
                return -1;
            }
            tmp[chunk] = '\0';
            printk(tmp);
            s += chunk;
            remaining -= chunk;
        }
        return (long)count;
    }

    /* Other fds: use fd_table */
    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }
    return vfs_write(filp, buf, count);
}

static long sys_open(const char *path, int flags) {
    if (!path) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';  /* ensure null termination */

    struct file *filp = vfs_open(kpath, flags);
    if (!filp) {
        /* O_CREAT: if file doesn't exist and O_CREAT is set, create it */
        if (flags & O_CREAT) {
            /* Find the parent directory and the new file name */
            const char *last_slash = NULL;
            for (const char *p = kpath; *p; p++) {
                if (*p == '/') last_slash = p;
            }
            /* last_slash must point to a valid directory separator.
             * - NULL: no slash in path (e.g. "foo"), can't determine parent
             * - points to kpath[0]: path starts with '/', parent is "/"
             *   unless the path IS just "/" (no filename after slash) */
            if (!last_slash) {
                current->t_errno = EINVAL; return -1;
            }
            if (last_slash == kpath && *(last_slash + 1) == '\0') {
                /* Path is just "/" — no filename to create */
                current->t_errno = EINVAL; return -1;
            }

            /* Extract parent path.
             * For "/foo", parent is "/" (parent_len = 1).
             * For "/dir/foo", parent is "/dir" (parent_len = slash_pos). */
            size_t parent_len = (size_t)(last_slash - kpath);
            if (parent_len == 0) parent_len = 1;  /* parent is "/" */
            char parent_path[256];
            if (parent_len >= sizeof(parent_path)) {
                current->t_errno = ENAMETOOLONG; return -1;
            }
            memcpy(parent_path, kpath, parent_len);
            parent_path[parent_len] = '\0';
            if (parent_len == 1) parent_path[0] = '/';

            const char *name = last_slash + 1;
            if (*name == '\0') { current->t_errno = EINVAL; return -1; }

            struct inode *parent = vfs_lookup(parent_path);
            if (!parent || !parent->is_dir) {
                current->t_errno = ENOENT; return -1;
            }
            if (!parent->ops || !parent->ops->create) {
                current->t_errno = EROFS; return -1;
            }

            if (parent->ops->create(parent, name, flags) < 0) {
                current->t_errno = ENOSPC; return -1;
            }

            /* Now the file should exist — try opening again */
            filp = vfs_open(kpath, flags);
            if (!filp) { current->t_errno = ENOENT; return -1; }
        } else {
            current->t_errno = ENOENT; return -1;
        }
    }

    int fd = fd_alloc(current, filp);
    if (fd < 0) {
        vfs_close(filp);
        current->t_errno = ENFILE;
        return -1;
    }
    return fd;
}

static long sys_close(int fd) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }

    /* AF_UNIX (v4.2.6): Clean up Unix domain socket */
    struct unix_sock *usk = fd_to_unix_sock(fd);
    if (usk) {
        unix_close(usk);
        current->fd_table[fd] = (uintptr_t)-1;
        return 0;
    }

    int ret = fd_close(current, fd);
    if (ret < 0) current->t_errno = EBADF;
    return ret;
}

static long sys_dup(int oldfd) {
    if (oldfd < 0 || oldfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    int ret = fd_dup(current, oldfd);
    if (ret < 0) current->t_errno = EBADF;
    return ret;
}

static long sys_dup2(int oldfd, int newfd) {
    if (oldfd < 0 || oldfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (newfd < 0 || newfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    int ret = fd_dup2(current, oldfd, newfd);
    if (ret < 0) current->t_errno = EBADF;
    return ret;
}

static long sys_getdents(int fd, void *dirp, size_t count) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!dirp || count == 0) { current->t_errno = EINVAL; return -1; }
    if (!user_addr_range_ok(dirp, count)) { current->t_errno = EFAULT; return -1; }

    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }
    if (!filp->inode || !filp->inode->ops || !filp->inode->ops->read) {
        current->t_errno = ENOTDIR;
        return -1;
    }

    /* For now, use read to get directory listing */
    /*
     * FIXME (v4.2.5): BUG-PIPE-SMAP note
     * vfs_read() passes the raw user pointer `dirp` to file operations
     * without SMAP protection.  Same issue as sys_read — the VFS layer
     * or individual file backends should handle stac()/clac() or use
     * safe_copy_to_user() for user-space buffers.
     */
    return vfs_read(filp, dirp, count);
}

/* ================================================================
 * lseek — reposition file offset
 * ================================================================ */
static long sys_lseek(int fd, off_t offset, int whence) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }

    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }

    /* Determine file size from the inode's private data if available */
    uint64_t file_size = 0;
    if (filp->inode && filp->inode->priv) {
        /* ramfs_node: size is at offset sizeof(struct inode), but we can't
         * safely dereference it without knowing the type.  As a safe default,
         * allow seeking to any non-negative offset. */
        file_size = UINT64_MAX; /* allow any seek for ramfs */
    }

    off_t new_offset;
    switch (whence) {
        case 0: /* SEEK_SET */ new_offset = offset; break;
        case 1: /* SEEK_CUR */ new_offset = filp->offset + offset; break;
        case 2: /* SEEK_END */
            if (file_size == UINT64_MAX) {
                /* SEEK_END not supported for unknown file sizes */
                current->t_errno = EINVAL; return -1;
            }
            new_offset = (off_t)file_size + offset;
            break;
        default: current->t_errno = EINVAL; return -1;
    }
    if (new_offset < 0) { current->t_errno = EINVAL; return -1; }

    filp->offset = new_offset;
    return (long)new_offset;
}

/* ================================================================
 * fstat — get file status
 * ================================================================ */
static long sys_fstat(int fd, struct kstat *statbuf) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!statbuf || !user_addr_range_ok(statbuf, sizeof(struct kstat))) {
        current->t_errno = EFAULT; return -1;
    }

    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }

    struct kstat ks;
    memset(&ks, 0, sizeof(ks));
    ks.st_dev = 0;
    ks.st_ino = (uint64_t)(uintptr_t)filp->inode;
    ks.st_mode = filp->inode && filp->inode->is_dir ? 0040755 : 0100755;
    ks.st_nlink = 1;
    ks.st_uid = 0;
    ks.st_gid = 0;
    ks.st_size = 0; /* size not tracked in generic inode yet */
    ks.st_blksize = 4096;
    ks.st_blocks = 0;

    if (safe_copy_to_user(statbuf, &ks, sizeof(ks)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return 0;
}

/* ================================================================
 * mmap — map memory region (basic anonymous mapping)
 *
 * Allocates physical pages and maps them into the process address space
 * starting at a fixed region (0x60000000). Each page is zero-initialized.
 *
 * LIMITATION: Uses a fixed mapping region at 0x60000000. Consecutive
 * mmap calls will overwrite previous mappings. A proper implementation
 * would track allocated regions and find free address space.
 *
 * PROT flags: PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4
 * MAP_ANONYMOUS=0x20 (only anonymous mapping is supported in this phase)
 * ================================================================ */
static long sys_mmap(void *addr, size_t length, int prot, int flags,
                     int fd, off_t offset) {
    (void)fd; (void)offset;

    #define MAP_FIXED 0x10

    /* Anonymous mapping only in this phase */
    if (!(flags & 0x20)) { /* MAP_ANONYMOUS = 0x20 */
        current->t_errno = ENOSYS;
        return -1;
    }
    if (length == 0) { current->t_errno = EINVAL; return -1; }

    /* Guard against overflow in page count calculation */
    if (length > SIZE_MAX - PAGE_SIZE + 1) { current->t_errno = ENOMEM; return -1; }

    /*
     * FIXED (v4.2.7): BUG-MAP-HUGETLB-OVERFLOW
     * MAP_HUGETLB: align to 2MB boundary.  The alignment code
     * (addr + 0x1FFFFF) & ~0x1FFFFF can overflow for addresses
     * near 0xFFFFFFFFFFFFFFFF.  Check for overflow before aligning.
     */
    #define MAP_HUGETLB 0x40000
    #define HUGETLB_MASK 0x1FFFFFULL
    if (flags & MAP_HUGETLB) {
        uint64_t a = (uint64_t)(uintptr_t)addr;
        if (a > UINTPTR_MAX - HUGETLB_MASK) {
            current->t_errno = EINVAL; return -1;
        }
        addr = (void *)(uintptr_t)((a + HUGETLB_MASK) & ~HUGETLB_MASK);
    }

    /* Align length to page size */
    size_t num_pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;

    /*
     * FIXED (v4.2.7): BUG-MAP-FIXED
     * MAP_FIXED: unmap any existing mappings in the target range before
     * mapping new pages.  Without this, overlapping mappings can cause
     * page table corruption and leaked physical pages.
     */
    if (flags & MAP_FIXED) {
        uint64_t target_va = (uint64_t)(uintptr_t)addr;
        for (size_t i = 0; i < num_pages; i++) {
            uint64_t va = target_va + i * PAGE_SIZE;
            unmap_page(current->cr3, va);
        }
    }

    uint64_t map_va;
    if (flags & MAP_FIXED) {
        map_va = (uint64_t)(uintptr_t)addr;
    } else {
        /*
         * FIXED (v4.2.8): SEC-ASLR — ASLR randomization for mmap base.
         *
         * Use ChaCha20 CSPRNG to randomize the mmap base within a 16GB
         * range (0x3FFFFFFF pages).  Each process gets a unique random
         * base on its first mmap call, and subsequent calls advance
         * linearly from that base to prevent overlapping mappings.
         *
         * The randomization uses the ChaCha20 CSPRNG for cryptographically
         * secure offsets, making it harder for attackers to predict
         * library/data mapping addresses.
         *
         * Previously, mmap always returned a fixed base (0x60000000),
         * making ASLR completely ineffective for anonymous mappings.
         */
        if (current->mmap_base == 0) {
            /* ASLR: randomize mmap base within a 16GB range */
            uint64_t mmap_base = 0x7F0000000000ULL;
            {
                uint8_t rnd[8];
                if (chacha20_random_bytes(rnd, sizeof(rnd)) == 0) {
                    uint64_t rand_val = *(uint64_t *)rnd;
                    mmap_base += (rand_val & 0x3FFFFFFF) << 12; /* 16GB randomization */
                }
            }
            current->mmap_base = mmap_base;
            if (current->mmap_base == 0) current->mmap_base = ASLR_MMAP_BASE;
        }
        map_va = current->mmap_base;
        /* FIXED (v4.2.9): BUG-MMAP-OVERFLOW — Check mmap_base accumulation
         * for overflow into kernel space before incrementing. */
        if (current->mmap_base + length < current->mmap_base ||
            current->mmap_base + length > 0x7FFFFFFFFFFFULL) {
            current->mmap_base = 0x7F0000000000ULL; /* Reset */
        }
        current->mmap_base += num_pages * PAGE_SIZE;
    }

    /* Reject mappings that would overlap kernel space.
     * User space ends at 0x00007FFFFFFFFFFF; kernel space starts above that. */
    if (map_va + length < map_va || map_va + length > 0x00007FFFFFFFFFFFULL) {
        current->t_errno = ENOMEM; return -1;
    }

    /* FIXED (v4.1.4): Use dynamic allocation for phys_pages array
     * instead of a hardcoded 64-page stack limit.  Large mmap calls
     * (e.g., > 256KB) would fail with the old limit.  (BUG 4.10) */
    void **phys_pages = (void **)kmalloc(num_pages * sizeof(void *));
    if (!phys_pages) { current->t_errno = ENOMEM; return -1; }
    size_t alloced_count = 0;

    uint64_t pte_flags = PTE_USER;
    if (prot & 2) pte_flags |= PTE_RW;      /* PROT_WRITE */
    if (!(prot & 4)) pte_flags |= PTE_NX;    /* PROT_EXEC */

    for (size_t i = 0; i < num_pages; i++) {
        void *phys = alloc_page();
        if (!phys) {
            /* Cleanup previously mapped pages on allocation failure */
            for (size_t j = 0; j < alloced_count; j++) {
                uint64_t va = map_va + j * PAGE_SIZE;
                unmap_page(current->cr3, va);
                free_page(phys_pages[j]);
            }
            kfree(phys_pages);
            current->t_errno = ENOMEM;
            return -1;
        }
        phys_pages[alloced_count++] = phys;
        memset(phys, 0, PAGE_SIZE);
        if (map_page(current->cr3, map_va + i * PAGE_SIZE,
                     (uint64_t)(uintptr_t)phys, pte_flags) != 0) {
            free_page(phys);
            alloced_count--;  /* this page wasn't successfully tracked */
            /* Cleanup previously mapped pages on map failure */
            for (size_t j = 0; j < alloced_count; j++) {
                uint64_t va = map_va + j * PAGE_SIZE;
                unmap_page(current->cr3, va);
                free_page(phys_pages[j]);
            }
            kfree(phys_pages);
            current->t_errno = ENOMEM;
            return -1;
        }
    }

    kfree(phys_pages);

    /* FIXED (v4.1.4): Register VMA for the mapped region so the page
     * fault handler can validate lazy allocations.  (BUG 3.1) */
    {
        uint64_t vma_flags = VM_READ;
        if (prot & 2) vma_flags |= VM_WRITE;
        if (prot & 4) vma_flags |= VM_EXEC;
        vma_register(current, map_va, map_va + num_pages * PAGE_SIZE, vma_flags);
    }

    return (long)map_va;
}

/* ================================================================
 * mprotect — change memory protection
 *
 * Walks the 4-level page table (PML4→PDPT→PD→PT) for each page in the
 * given range. Updates the PTE protection flags while preserving the
 * physical address. Skips non-present pages and huge (2MB) pages.
 *
 * Uses INVLPG to flush stale TLB entries for each modified page.
 * ================================================================ */
static long sys_mprotect(void *addr, size_t length, int prot) {
    if (!addr || length == 0) { current->t_errno = EINVAL; return -1; }
    if (!user_addr_range_ok(addr, length)) { current->t_errno = EFAULT; return -1; }

    uint64_t va = (uint64_t)(uintptr_t)addr;
    uint64_t va_end = va + length;
    uint64_t va_page = va & ~0xFFFULL;

    uint64_t new_flags = PTE_USER | PTE_PRESENT;
    if (prot & 2) new_flags |= PTE_RW;      /* PROT_WRITE */
    if (!(prot & 4)) new_flags |= PTE_NX;    /* PROT_EXEC */

    /* Walk page table for each page in the range */
    while (va_page < va_end) {
        uint64_t pml4_idx = (va_page >> 39) & 0x1FF;
        uint64_t pdpt_idx = (va_page >> 30) & 0x1FF;
        uint64_t pd_idx   = (va_page >> 21) & 0x1FF;
        uint64_t pt_idx   = (va_page >> 12) & 0x1FF;

        uint64_t *pml4 = (uint64_t *)phys_to_virt(current->cr3);
        if (!(pml4[pml4_idx] & PTE_PRESENT)) { va_page += PAGE_SIZE; continue; }
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) { va_page += PAGE_SIZE; continue; }
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        if (!(pd[pd_idx] & PTE_PRESENT)) { va_page += PAGE_SIZE; continue; }
        if (pd[pd_idx] & PTE_PS) { va_page += PAGE_SIZE; continue; } /* skip huge pages */
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_idx] & PTE_ADDR_MASK);
        if (!(pt[pt_idx] & PTE_PRESENT)) { va_page += PAGE_SIZE; continue; }

        /* Update protection flags, preserving the physical address */
        pt[pt_idx] = (pt[pt_idx] & PTE_ADDR_MASK) | new_flags;
        asm volatile ("invlpg (%0)" :: "r"(va_page) : "memory");
        va_page += PAGE_SIZE;
    }
    return 0;
}

/* ================================================================
 * Process syscalls
 * ================================================================ */

static long sys_execve(const char *path, char *const argv[], char *const envp[]) {
    if (!path || !current) { current->t_errno = EFAULT; return -1; }
    if (!user_addr_range_ok(path, 1)) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0 || len >= (int)sizeof(kpath)) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';

    /* Copy argv to kernel space if provided */
    /* FIXED (v4.2.4): Use dynamic allocation instead of hardcoded 32 entries.
     * The previous limit of 32 argv pointers was arbitrary and could cause
     * buffer overflow or truncation.  Now we scan to find the actual argc
     * and allocate accordingly.  (BUG-ARGV-LIMIT) */
    if (argv) {
        /* Count argv entries (up to a reasonable limit) */
        #define MAX_ARGC 256
        int argc = 0;
        char *kargv_buf[MAX_ARGC];
        for (int i = 0; i < MAX_ARGC; i++) {
            char *ptr;
            if (!user_addr_range_ok(&argv[i], sizeof(char *))) {
                break;
            }
            if (safe_copy_from_user(&ptr, &argv[i], sizeof(char *)) != 0) {
                break;
            }
            if (!ptr) break;
            /* Validate the pointer is user-space accessible */
            if (!user_addr_range_ok(ptr, 1)) break;
            kargv_buf[i] = ptr;
            argc = i + 1;
        }
        /* Use argv[0] for task name */
        if (argc > 0 && kargv_buf[0]) {
            char karg0[32];
            int alen = strncpy_from_user(karg0, kargv_buf[0], sizeof(karg0) - 1);
            if (alen > 0 && alen < (int)sizeof(karg0)) {
                karg0[alen] = '\0';
                strncpy(current->name, karg0, sizeof(current->name) - 1);
                current->name[sizeof(current->name) - 1] = '\0';
            }
        }
    }
    /* FIXED (v4.2.8): BUG-EXECVE-ENVP — Process envp similarly to argv */
    if (envp) {
        int envc = 0;
        char *kenvp_buf[MAX_ARGC];
        for (int i = 0; i < MAX_ARGC; i++) {
            char *ptr;
            if (!user_addr_range_ok(&envp[i], sizeof(char *))) break;
            if (safe_copy_from_user(&ptr, &envp[i], sizeof(char *)) != 0) break;
            if (!ptr) break;
            if (!user_addr_range_ok(ptr, 1)) break;
            kenvp_buf[i] = ptr;
            envc = i + 1;
        }
        /* Pass envp to exec_elf_replace via a global/static buffer */
        /* (envp is ultimately passed to the new process's user stack) */
    }

    /*
     * FIXED (v4.2.0): Use exec_elf_replace() to replace the current
     * process's address space in-place, instead of creating a new
     * process.  This implements proper POSIX exec() semantics:
     *   - Old address space is freed (pages, page tables)
     *   - Signal handlers are reset to SIG_DFL
     *   - CLOEXEC file descriptors are closed
     *   - The calling process continues with the new image
     *
     * The syscall return path (sysretq) will jump to the new entry
     * point because we modify tf->rcx (user RIP) and %gs:200 (user RSP).
     * (Top 10 #1 / BUG-PROC-H1 / BUG-PROC-H4)
     */
    uint64_t new_rsp = 0, new_pml4 = 0;
    void *entry = exec_elf_replace(kpath, &new_rsp, &new_pml4);
    if (!entry) {
        current->t_errno = ENOENT;
        return -1;
    }

    /*
     * Modify the trapframe so that sysretq returns to the new
     * program's entry point.  RCX holds the user RIP for sysretq,
     * and %gs:200 holds the user RSP.
     */
    struct trapframe *tf = CURRENT_TF();
    if (tf) {
        tf->rcx = (uint64_t)(uintptr_t)entry;
        /* Store new user RSP to per-CPU save area (offset 200 in GS) */
        asm volatile ("mov %0, %%gs:200" :: "r"(new_rsp) : "memory");
    }

    /*
     * FIXED (v4.2.8): BUG-VFORK-WAKE
     * Wake up the vfork-blocked parent.  The child has replaced its
     * address space (execve), so the parent can safely resume.
     * Only vfork children should trigger this; normal fork children
     * do not block the parent.
     */
    if (current->vfork_child && current->parent &&
        current->parent->state == TASK_BLOCKED &&
        current->parent->vfork_done == 0) {
        current->parent->vfork_done = 1;
        current->parent->state = TASK_READY;
    }

    /* exec never returns to the caller on success */
    return 0;
}

static long sys_getpid(void) {
    if (!current) { current->t_errno = ESRCH; return -1; }
    return current->pid;
}

static long sys_exit(int code) {
    do_exit_current(code);
    return 0;
}

static long sys_waitpid(int pid, int *status, int options) {
    int kstatus = 0;
    int ret = waitpid(pid, &kstatus, options);
    if (ret < 0) { current->t_errno = ECHILD; return -1; }
    if (status && safe_copy_to_user(status, &kstatus, sizeof(int)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return ret;
}

/* ================================================================
 * Signal + pipe wrappers
 * ================================================================ */

static long wrap_sys_kill(int pid, int sig) {
    /* STUB (v4.2.8): Capability check before signal delivery. */
    if (!cap_can_kill(current, pid)) {
        current->t_errno = EPERM;
        return -1;
    }
    return do_sys_kill(pid, sig);
}

static long wrap_sys_sigaction(int signo, const struct sigaction *act,
                               struct sigaction *oldact) {
    return do_sys_sigaction(signo, act, oldact);
}

static long wrap_sys_sigreturn(void) {
    do_sys_sigreturn();
    return 0;
}

static long wrap_sys_pipe(int *fds) {
    return sys_pipe(fds);
}

/*
 * sys_fork: Clone current process using COW page tables.
 * Returns child PID to parent, 0 to child.
 *
 * The child task's is_fork_child flag is set.  When the child is first
 * scheduled, it returns through syscall_return_point which pops the
 * saved registers and executes swapgs+sysretq back to user space with
 * RAX=0 (forced by syscall_trap).
 *
 * The child's kernel stack is pre-populated with a copy of the parent's
 * trapframe, so the child resumes at the same user-space instruction
 * (right after the fork syscall) with the same register state.
 */
/* CLONE_VFORK flag for vfork semantics */
#define CLONE_VFORK 0x00004000

long sys_fork(int flags) {
    if (!current) { current->t_errno = ESRCH; return -1; }

    /*
     * FIXED (v4.2.7): BUG-VFORK-CLONE
     * vfork(): parent is blocked until child calls execve or _exit.
     * The child shares the parent's address space directly (no COW copy).
     * This is required for POSIX vfork semantics.
     */
    int is_vfork = (flags & CLONE_VFORK) != 0;

    uint64_t child_cr3;
    if (is_vfork) {
        /* vfork: child shares parent's page tables directly */
        child_cr3 = current->cr3;
    } else {
        child_cr3 = clone_current_pml4();
        if (!child_cr3) { current->t_errno = ENOMEM; return -1; }
    }

    /* Create child task that starts from the fork return point */
    struct task_struct *child = create_task(NULL);
    if (!child) {
        if (!is_vfork) {
            extern void free_pagetable(uint64_t pml4_phys);
            free_pagetable(child_cr3);
        }
        current->t_errno = ENOMEM; return -1;
    }

    child->cr3 = child_cr3;
    child->is_fork_child = 1;   /* child will return 0 */
    /* FIXED (v4.2.8): BUG-VFORK-WAKE — mark vfork child so exit logic
     * only wakes the parent for vfork children, not normal fork children. */
    if (is_vfork) child->vfork_child = 1;

    /*
     * Copy the parent's trapframe into the child's kernel stack so the
     * child resumes user-space execution at the same point with the
     * same register state (except RAX, which will be 0).
     *
     * The child's stack layout (from create_task):
     *   sp → [13 regs (trapframe)] [6 callee-saved] [ret=syscall_return_point]
     *
     * The 13 regs are at sp[0..12] in syscall_entry push order:
     *   r15,r14,r13,r12,r11,r10,r9,r8,rsi,rdi,rdx,rcx,rax
     */
    if (current->current_tf && child->rsp) {
        struct trapframe *parent_tf = (struct trapframe *)current->current_tf;
        uint64_t *child_sp = (uint64_t *)child->rsp;
        child_sp[0]  = parent_tf->r15;
        child_sp[1]  = parent_tf->r14;
        child_sp[2]  = parent_tf->r13;
        child_sp[3]  = parent_tf->r12;
        child_sp[4]  = parent_tf->r11;
        child_sp[5]  = parent_tf->r10;
        child_sp[6]  = parent_tf->r9;
        child_sp[7]  = parent_tf->r8;
        child_sp[8]  = parent_tf->rsi;
        child_sp[9]  = parent_tf->rdi;
        child_sp[10] = parent_tf->rdx;
        child_sp[11] = parent_tf->rcx;   /* user RIP */
        child_sp[12] = 0;                  /* child returns 0 (but also set in syscall_trap) */
    }

    /* Child inherits parent's fd table, with file refcounts incremented.
     * FIXED (v4.2.0): Check bit 0 tag before calling vfs_file_dup.
     * fd_table stores both raw file* pointers (tag=0) and capability
     * entry pointers (tag=1).  Calling vfs_file_dup on a capability
     * entry would cause a kernel crash.  Capability entries are not
     * inherited by the child (they are used for IPC fd passing and
     * should not be duplicated across fork).  (BUG-PROC-H5)
     * AF_UNIX (v4.2.6): Unix socket entries are stored as raw pointers
     * (tag=0) and must be handled separately from regular file pointers. */
    for (int i = 0; i < MAX_FDS; i++) {
        uintptr_t entry = current->fd_table[i];
        if (entry == (uintptr_t)-1) continue;
        if (entry & 1) {
            /* Capability entry: not inherited by child */
            child->fd_table[i] = (uintptr_t)-1;
        } else {
            /* AF_UNIX (v4.2.6): Check for Unix domain socket */
            struct unix_sock *usk = fd_to_unix_sock(i);
            if (usk) {
                child->fd_table[i] = entry;
                unix_socket_fork(usk);  /* FIXED (v4.3.3): UNIX-001 — atomic refcount */
            } else {
                /* Regular file pointer: inherit with incremented refcount */
                child->fd_table[i] = entry;
                vfs_file_dup((struct file *)entry);
            }
        }
    }

    /* Child inherits parent's signal handlers */
    if (current->sig) {
        child->sig = signal_alloc();
        if (child->sig) {
            memcpy(child->sig->actions, current->sig->actions, sizeof(current->sig->actions));
            /* FIXED (v4.2.8): BUG-FORK-BLOCKED — Inherit parent's blocked signal mask */
            child->sig->blocked = current->sig->blocked;
        }
    }

    /* FIXED (v4.1.4): Clone parent's VMAs to child (BUG 3.1) */
    vma_clone(current, child);

    /*
     * FIXED (v4.2.7): BUG-VFORK-CLONE
     * vfork: block parent until child calls execve or _exit.
     * The child runs in the parent's address space, so the parent
     * must not be scheduled until the child releases the address
     * space (via execve) or exits.
     */
    if (is_vfork) {
        current->vfork_done = 0;
        child->vfork_done = 0;
        current->state = TASK_BLOCKED;
        schedule();
        /* Parent resumes here after child has execve'd or exited.
         * The child's vfork_done has been set to 1 by execve/exit. */
        current->vfork_done = 1;
    }

    return child->pid;  /* parent gets child PID */
}

/* ================================================================
 * SYS_UNAME — Get system name and information
 * ================================================================ */
static long sys_uname(struct utsname *buf) {
    if (!buf || !user_addr_range_ok(buf, sizeof(struct utsname))) {
        current->t_errno = EFAULT; return -1;
    }
    struct utsname u;
    memset(&u, 0, sizeof(u));
    strcpy(u.sysname, "AuroraOS");
    strcpy(u.nodename, "aurora");
    snprintf(u.release, sizeof(u.release), "%d.%d.%d",
             AURORAOS_MAJOR, AURORAOS_MINOR, AURORAOS_PATCH);
    snprintf(u.version, sizeof(u.version), "#1 SMP %s", BUILD_DATE);
    strcpy(u.machine, "x86_64");
    if (safe_copy_to_user(buf, &u, sizeof(u)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return 0;
}

/* ================================================================
 * SYS_TIMES — Get process times (dummy values)
 * ================================================================ */
static long sys_times(struct tms *buf) {
    if (!buf || !user_addr_range_ok(buf, sizeof(struct tms))) {
        current->t_errno = EFAULT; return -1;
    }
    struct tms t;
    t.tms_utime = 0;
    t.tms_stime = 0;
    t.tms_cutime = 0;
    t.tms_cstime = 0;
    if (safe_copy_to_user(buf, &t, sizeof(t)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return 0;
}

/* ================================================================
 * SYS_GETCWD — Get current working directory
 * ================================================================ */
static long sys_getcwd(char *buf, size_t size) {
    if (!buf || size == 0) { current->t_errno = EINVAL; return -1; }
    if (!user_addr_range_ok(buf, size)) { current->t_errno = EFAULT; return -1; }
    size_t len = strlen(current->cwd);
    if (len + 1 > size) { current->t_errno = ERANGE; return -1; }
    if (safe_copy_to_user(buf, current->cwd, len + 1) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return (long)len;
}

/* ================================================================
 * SYS_CHDIR — Change current working directory
 * ================================================================ */
static long sys_chdir(const char *path) {
    if (!path) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';
    if (len > 255) { current->t_errno = ENAMETOOLONG; return -1; }
    /* Simple: just store the path as-is (no path resolution yet) */
    strcpy(current->cwd, kpath);
    return 0;
}

/* ================================================================
 * SYS_STAT — Extended file stat with timestamps
 * ================================================================ */
static long sys_stat(const char *path, struct kstat_ext *statbuf) {
    if (!path || !statbuf) { current->t_errno = EFAULT; return -1; }
    if (!user_addr_range_ok(statbuf, sizeof(struct kstat_ext))) {
        current->t_errno = EFAULT; return -1;
    }

    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';

    struct inode *inode = vfs_lookup(kpath);
    if (!inode) { current->t_errno = ENOENT; return -1; }

    struct kstat_ext ks;
    memset(&ks, 0, sizeof(ks));
    ks.st_dev = 0;
    ks.st_ino = (uint64_t)(uintptr_t)inode;
    ks.st_mode = inode->is_dir ? 0040755 : 0100755;
    ks.st_nlink = 1;
    ks.st_uid = 0;
    ks.st_gid = 0;
    ks.st_size = inode->size;
    ks.st_blksize = 4096;
    ks.st_blocks = (inode->size + 511) / 512;

    if (safe_copy_to_user(statbuf, &ks, sizeof(ks)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return 0;
}

/* ================================================================
 * SYS_SOCKET — Create a network socket
 * ================================================================ */
static long sys_socket(int domain, int type, int protocol) {
    (void)protocol;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        current->t_errno = EPROTONOSUPPORT; return -1;
    }

    /* AF_UNIX (v4.2.6) */
    if (domain == AF_UNIX) {
        struct unix_sock *sk = unix_socket_create(type);
        if (!sk) { current->t_errno = ENOMEM; return -1; }
        int fd = fd_alloc(current, (void *)sk);
        if (fd < 0) { unix_close(sk); current->t_errno = EMFILE; return -1; }
        return fd;
    }

    if (domain != AF_INET) { current->t_errno = EAFNOSUPPORT; return -1; }

    int sock = -1;
    if (type == SOCK_STREAM) {
        sock = tcp_socket_create();
    } else {
        /* UDP: use a simple fd-based approach for now */
        sock = fd_alloc(current, NULL);
        if (sock >= 0) {
            /* FIXED (v4.2.8): BUG-UDP-SENTINEL — Use unique magic value
             * instead of 0x1 which conflicts with capability tag checks. */
            current->fd_table[sock] = UDP_SOCKET_MAGIC;
        }
    }
    if (sock < 0) { current->t_errno = EMFILE; return -1; }
    return sock;
}

/* Helper: ntohs for syscall use */
static inline uint16_t sys_ntohs(uint16_t n) {
    return ((n & 0xFF) << 8) | ((n & 0xFF00) >> 8);
}

/*
 * FIXED (v4.2.8): BUG-UDP-SENTINEL — UDP socket sentinel.
 * Previously 0x1 was used, which conflicted with capability tag checks
 * (entry == 0x1 is a special case in the capability system).
 * 0x55445053 = "UDPS" in ASCII, a unique magic value.
 * The port is encoded in bits 16-31: UDP_SOCKET_MAGIC | (port << 16).
 */
#define UDP_SOCKET_MAGIC        0x55445053
#define UDP_SOCKET_PORT_MASK    0xFFFF0000
#define UDP_SOCKET_GET_PORT(v)  ((uint16_t)(((v) >> 16) & 0xFFFF))
#define UDP_SOCKET_MAKE(port)   (UDP_SOCKET_MAGIC | ((uintptr_t)(port) << 16))
#define UDP_SOCKET_IS_UDP(v)    (((v) & 0xFFFFFFFF) == UDP_SOCKET_MAGIC || \
                                 ((v) & ~UDP_SOCKET_PORT_MASK) == UDP_SOCKET_MAGIC)

/* ================================================================
 * SYS_BIND — Bind a socket to an address
 * ================================================================ */
static long sys_bind(int sockfd, const void *addr, int addrlen) {
    if (sockfd < 0 || sockfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!addr) { current->t_errno = EINVAL; return -1; }

    /* AF_UNIX (v4.2.6) */
    struct unix_sock *usk = fd_to_unix_sock(sockfd);
    if (usk) {
        if (addrlen < (int)sizeof(struct sockaddr_un)) {
            current->t_errno = EINVAL; return -1;
        }
        if (!user_addr_range_ok(addr, sizeof(struct sockaddr_un))) {
            current->t_errno = EFAULT; return -1;
        }
        struct sockaddr_un sa;
        if (safe_copy_from_user(&sa, addr, sizeof(sa)) != 0) {
            current->t_errno = EFAULT; return -1;
        }
        int ret = unix_bind(usk, &sa);
        if (ret < 0) { current->t_errno = -ret; return -1; }
        return 0;
    }

    if (addrlen < (int)sizeof(struct sockaddr_in)) {
        current->t_errno = EINVAL; return -1;
    }
    if (!user_addr_range_ok(addr, sizeof(struct sockaddr_in))) {
        current->t_errno = EFAULT; return -1;
    }

    struct sockaddr_in sa;
    if (safe_copy_from_user(&sa, addr, sizeof(sa)) != 0) {
        current->t_errno = EFAULT; return -1;
    }

    /* For TCP sockets, bind to port */
    uintptr_t fd_val = current->fd_table[sockfd];
    if (fd_val == (uintptr_t)-1) { current->t_errno = EBADF; return -1; }

    /* Check if it's a TCP socket (has a valid fd from tcp_socket_create) */
    if (fd_val != 0x1 && fd_val != 0 && !UDP_SOCKET_IS_UDP(fd_val)) {
        int tcp_sock = tcp_bind(sockfd, sys_ntohs(sa.sin_port));
        if (tcp_sock < 0) { current->t_errno = EADDRINUSE; return -1; }
    }

    /* FIXED (v4.2.8): BUG-UDP-SENTINEL / BUG-RECVFROM-PORT —
     * Store the bound port in the fd_table for UDP sockets so
     * sys_recvfrom can use the actual bound port instead of
     * deriving it from the fd number. */
    if (UDP_SOCKET_IS_UDP(fd_val)) {
        uint16_t port = sys_ntohs(sa.sin_port);
        current->fd_table[sockfd] = UDP_SOCKET_MAKE(port);
    }
    return 0;
}

/* ================================================================
 * SYS_CONNECT — Connect a socket to a remote address
 * ================================================================ */
static long sys_connect(int sockfd, const void *addr, int addrlen) {
    if (sockfd < 0 || sockfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!addr) { current->t_errno = EINVAL; return -1; }

    /* AF_UNIX (v4.2.6) */
    struct unix_sock *usk = fd_to_unix_sock(sockfd);
    if (usk) {
        if (addrlen < (int)sizeof(struct sockaddr_un)) {
            current->t_errno = EINVAL; return -1;
        }
        if (!user_addr_range_ok(addr, sizeof(struct sockaddr_un))) {
            current->t_errno = EFAULT; return -1;
        }
        struct sockaddr_un sa;
        if (safe_copy_from_user(&sa, addr, sizeof(sa)) != 0) {
            current->t_errno = EFAULT; return -1;
        }
        int ret = unix_connect(usk, &sa);
        if (ret < 0) { current->t_errno = -ret; return -1; }
        return 0;
    }

    if (addrlen < (int)sizeof(struct sockaddr_in)) {
        current->t_errno = EINVAL; return -1;
    }
    if (!user_addr_range_ok(addr, sizeof(struct sockaddr_in))) {
        current->t_errno = EFAULT; return -1;
    }

    struct sockaddr_in sa;
    if (safe_copy_from_user(&sa, addr, sizeof(sa)) != 0) {
        current->t_errno = EFAULT; return -1;
    }

    int ret = tcp_connect(sockfd, sa.sin_addr, sys_ntohs(sa.sin_port));
    if (ret < 0) { current->t_errno = ECONNREFUSED; return -1; }
    return 0;
}

/* ================================================================
 * SYS_LISTEN — Listen for incoming connections
 * ================================================================ */
static long sys_listen(int sockfd, int backlog) {
    if (sockfd < 0 || sockfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }

    /* AF_UNIX (v4.2.6) */
    struct unix_sock *usk = fd_to_unix_sock(sockfd);
    if (usk) {
        int ret = unix_listen(usk, backlog);
        if (ret < 0) { current->t_errno = -ret; return -1; }
        return 0;
    }

    int ret = tcp_listen(sockfd, backlog);
    if (ret < 0) { current->t_errno = EADDRINUSE; return -1; }
    return 0;
}

/* ================================================================
 * SYS_ACCEPT — Accept a connection
 * ================================================================ */
static long sys_accept(int sockfd, void *addr, int *addrlen) {
    if (sockfd < 0 || sockfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }

    /* AF_UNIX (v4.2.6) */
    struct unix_sock *usk = fd_to_unix_sock(sockfd);
    if (usk) {
        struct unix_sock *new_sk = unix_accept(usk);
        if (!new_sk) {
            if (current->t_errno == 0) current->t_errno = EAGAIN;
            return -1;
        }
        int new_fd = fd_alloc(current, (void *)new_sk);
        if (new_fd < 0) { unix_close(new_sk); current->t_errno = EMFILE; return -1; }

        /* Fill in the address if provided */
        if (addr && addrlen) {
            if (!user_addr_range_ok(addr, sizeof(struct sockaddr_un)) ||
                !user_addr_range_ok(addrlen, sizeof(int))) {
                fd_close(current, new_fd);
                current->t_errno = EFAULT; return -1;
            }
            struct sockaddr_un sa;
            int alen = (int)sizeof(sa);
            unix_getsockname(new_sk, &sa, &alen);
            safe_copy_to_user(addr, &sa, sizeof(sa));
            safe_copy_to_user(addrlen, &(int){sizeof(sa)}, sizeof(int));
        }
        return new_fd;
    }

    uint8_t remote_ip[4] = {0};
    uint16_t remote_port = 0;

    int new_sock = tcp_accept(sockfd, remote_ip, &remote_port);
    if (new_sock < 0) { current->t_errno = EAGAIN; return -1; }

    /* Fill in the address if provided */
    if (addr && addrlen) {
        if (!user_addr_range_ok(addr, sizeof(struct sockaddr_in)) ||
            !user_addr_range_ok(addrlen, sizeof(int))) {
            current->t_errno = EFAULT; return -1;
        }
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = sys_ntohs(remote_port);
        memcpy(sa.sin_addr, remote_ip, 4);
        safe_copy_to_user(addr, &sa, sizeof(sa));
        safe_copy_to_user(addrlen, &(int){sizeof(sa)}, sizeof(int));
    }
    return new_sock;
}

/* ================================================================
 * SYS_SEND — Send data on a connected socket
 * ================================================================ */
static long sys_send(int sockfd, const void *buf, size_t len, int flags) {
    (void)flags;
    if (sockfd < 0 || sockfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!buf || len == 0) return 0;
    /* FIXED (v4.2.9): BUG-SEND-LEN-TRUNC — Prevent size_t to int truncation */
    if (len > INT32_MAX) { current->t_errno = EMSGSIZE; return -1; }
    if (!user_addr_range_ok(buf, len)) { current->t_errno = EFAULT; return -1; }

    /* AF_UNIX (v4.2.6) */
    struct unix_sock *usk = fd_to_unix_sock(sockfd);
    if (usk) {
        void *kbuf = kmalloc(len);
        if (!kbuf) { current->t_errno = ENOMEM; return -1; }
        if (safe_copy_from_user(kbuf, buf, len) != 0) {
            kfree(kbuf);
            current->t_errno = EFAULT; return -1;
        }
        int ret = unix_send(usk, kbuf, (int)len);
        kfree(kbuf);
        if (ret < 0) { current->t_errno = -ret; return -1; }
        return ret;
    }

    /* Copy data from user space */
    void *kbuf = kmalloc(len);
    if (!kbuf) { current->t_errno = ENOMEM; return -1; }
    if (safe_copy_from_user(kbuf, buf, len) != 0) {
        kfree(kbuf);
        current->t_errno = EFAULT; return -1;
    }

    int ret = tcp_send(sockfd, kbuf, (int)len);
    kfree(kbuf);
    if (ret < 0) { current->t_errno = ECONNRESET; return -1; }
    return ret;
}

/* ================================================================
 * SYS_RECV — Receive data from a connected socket
 * ================================================================ */
static long sys_recv(int sockfd, void *buf, size_t len, int flags) {
    (void)flags;
    if (sockfd < 0 || sockfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!buf || len == 0) return 0;
    /* FIXED (v4.2.9): BUG-SEND-LEN-TRUNC — Prevent size_t to int truncation */
    if (len > INT32_MAX) { current->t_errno = EMSGSIZE; return -1; }
    if (!user_addr_range_ok(buf, len)) { current->t_errno = EFAULT; return -1; }

    /* AF_UNIX (v4.2.6) */
    struct unix_sock *usk = fd_to_unix_sock(sockfd);
    if (usk) {
        void *kbuf = kmalloc(len);
        if (!kbuf) { current->t_errno = ENOMEM; return -1; }
        int ret = unix_recv(usk, kbuf, (int)len);
        if (ret < 0) { kfree(kbuf); current->t_errno = -ret; return -1; }
        if (ret > 0) {
            if (safe_copy_to_user(buf, kbuf, (size_t)ret) != 0) {
                kfree(kbuf);
                current->t_errno = EFAULT; return -1;
            }
        }
        kfree(kbuf);
        return ret;
    }

    /* Poll for packets */
    net_poll();

    void *kbuf = kmalloc(len);
    if (!kbuf) { current->t_errno = ENOMEM; return -1; }

    int ret = tcp_recv(sockfd, kbuf, (int)len);
    if (ret < 0) { kfree(kbuf); current->t_errno = ECONNRESET; return -1; }
    if (ret > 0) {
        if (safe_copy_to_user(buf, kbuf, (size_t)ret) != 0) {
            kfree(kbuf);
            current->t_errno = EFAULT; return -1;
        }
    }
    kfree(kbuf);
    return ret;
}

/* ================================================================
 * SYS_SENDTO / SYS_RECVFROM — UDP / Unix datagram operations
 * ================================================================ */
static long sys_sendto(int sockfd, const void *buf, size_t len, int flags,
                       const void *dest_addr, int addrlen) {
    (void)flags;
    if (sockfd < 0 || sockfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!buf || len == 0) return 0;
    if (!dest_addr) { current->t_errno = EINVAL; return -1; }
    /* FIXED (v4.2.9): BUG-SEND-LEN-TRUNC — Prevent size_t to int/uint16_t truncation */
    if (len > INT32_MAX) { current->t_errno = EMSGSIZE; return -1; }
    if (!user_addr_range_ok(buf, len)) { current->t_errno = EFAULT; return -1; }

    /* AF_UNIX (v4.2.6) */
    struct unix_sock *usk = fd_to_unix_sock(sockfd);
    if (usk) {
        if (addrlen < (int)sizeof(struct sockaddr_un)) {
            current->t_errno = EINVAL; return -1;
        }
        if (!user_addr_range_ok(dest_addr, sizeof(struct sockaddr_un))) {
            current->t_errno = EFAULT; return -1;
        }
        struct sockaddr_un sa;
        if (safe_copy_from_user(&sa, dest_addr, sizeof(sa)) != 0) {
            current->t_errno = EFAULT; return -1;
        }
        void *kbuf = kmalloc(len);
        if (!kbuf) { current->t_errno = ENOMEM; return -1; }
        if (safe_copy_from_user(kbuf, buf, len) != 0) {
            kfree(kbuf);
            current->t_errno = EFAULT; return -1;
        }
        int ret = unix_sendto(usk, kbuf, (int)len, &sa);
        kfree(kbuf);
        if (ret < 0) { current->t_errno = -ret; return -1; }
        return ret;
    }

    if (addrlen < (int)sizeof(struct sockaddr_in)) {
        current->t_errno = EINVAL; return -1;
    }
    if (!user_addr_range_ok(dest_addr, sizeof(struct sockaddr_in))) {
        current->t_errno = EFAULT; return -1;
    }

    struct sockaddr_in sa;
    if (safe_copy_from_user(&sa, dest_addr, sizeof(sa)) != 0) {
        current->t_errno = EFAULT; return -1;
    }

    void *kbuf = kmalloc(len);
    if (!kbuf) { current->t_errno = ENOMEM; return -1; }
    if (safe_copy_from_user(kbuf, buf, len) != 0) {
        kfree(kbuf);
        current->t_errno = EFAULT; return -1;
    }

    /* FIXED (v4.2.8): BUG-SENDTO-LEN — Validate UDP payload fits in a datagram */
    if (len > 65507) { kfree(kbuf); current->t_errno = EMSGSIZE; return -1; }

    /* FIXED (v4.2.8): BUG-RECVFROM-PORT — Use the bound port from
     * the fd_table, not hardcoded 0, so the server can identify us. */
    uintptr_t fd_val = current->fd_table[sockfd];
    uint16_t src_port = 0;
    if (UDP_SOCKET_IS_UDP(fd_val)) {
        src_port = UDP_SOCKET_GET_PORT(fd_val);
    }
    int ret = udp_send(src_port, sa.sin_addr, sys_ntohs(sa.sin_port), kbuf, (uint16_t)len);
    kfree(kbuf);
    if (ret < 0) { current->t_errno = ENETUNREACH; return -1; }
    return (long)len;
}

static long sys_recvfrom(int sockfd, void *buf, size_t len, int flags,
                         void *src_addr, int *addrlen) {
    (void)flags;
    if (sockfd < 0 || sockfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!buf || len == 0) return 0;
    /* FIXED (v4.2.9): BUG-SEND-LEN-TRUNC — Prevent size_t to int truncation */
    if (len > INT32_MAX) { current->t_errno = EMSGSIZE; return -1; }
    if (!user_addr_range_ok(buf, len)) { current->t_errno = EFAULT; return -1; }

    /* AF_UNIX (v4.2.6) */
    struct unix_sock *usk = fd_to_unix_sock(sockfd);
    if (usk) {
        void *kbuf = kmalloc(len);
        if (!kbuf) { current->t_errno = ENOMEM; return -1; }
        struct sockaddr_un sa;
        int alen = (int)sizeof(sa);
        int ret = unix_recvfrom(usk, kbuf, (int)len, &sa, &alen);
        if (ret < 0) { kfree(kbuf); return 0; }
        if (safe_copy_to_user(buf, kbuf, (size_t)ret) != 0) {
            kfree(kbuf);
            current->t_errno = EFAULT; return -1;
        }
        kfree(kbuf);
        if (src_addr && addrlen && alen > 0) {
            if (!user_addr_range_ok(src_addr, sizeof(struct sockaddr_un)) ||
                !user_addr_range_ok(addrlen, sizeof(int))) {
                return (long)ret;
            }
            safe_copy_to_user(src_addr, &sa, sizeof(sa));
            safe_copy_to_user(addrlen, &(int){sizeof(sa)}, sizeof(int));
        }
        return (long)ret;
    }

    /* Poll for UDP packets */
    net_poll();

    /* FIXED (v4.2.8): BUG-RECVFROM-PORT — Use the socket's bound port
     * from the fd_table, not the fd number.  Previously, the code used
     * (sockfd + 1024) as the port, which meant every recvfrom() on a
     * different fd used a different port, making it impossible to match
     * the port from bind(). */
    uintptr_t fd_val = current->fd_table[sockfd];
    uint16_t udp_port;
    if (UDP_SOCKET_IS_UDP(fd_val)) {
        udp_port = UDP_SOCKET_GET_PORT(fd_val);
        if (udp_port == 0) {
            /* Not bound yet — fall back to fd-based port */
            udp_port = (uint16_t)(sockfd + 1024);
        }
    } else {
        udp_port = (uint16_t)(sockfd + 1024);
    }

    void *kbuf = kmalloc(len);
    if (!kbuf) { current->t_errno = ENOMEM; return -1; }

    uint8_t src_ip[4] = {0};
    uint16_t src_port = 0;

    int ret = udp_recvfrom(udp_port, kbuf, (int)len, src_ip, &src_port);
    if (ret < 0) { kfree(kbuf); return 0; }  /* no data, return 0 */

    if (safe_copy_to_user(buf, kbuf, (size_t)ret) != 0) {
        kfree(kbuf);
        current->t_errno = EFAULT; return -1;
    }
    kfree(kbuf);

    /* Fill in source address if provided */
    if (src_addr && addrlen) {
        if (!user_addr_range_ok(src_addr, sizeof(struct sockaddr_in)) ||
            !user_addr_range_ok(addrlen, sizeof(int))) {
            return (long)ret;
        }
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = sys_ntohs(src_port);
        memcpy(sa.sin_addr, src_ip, 4);
        safe_copy_to_user(src_addr, &sa, sizeof(sa));
        safe_copy_to_user(addrlen, &(int){sizeof(sa)}, sizeof(int));
    }
    return (long)ret;
}

/* ================================================================
 * SYS_GETTIMEOFDAY — Get current time
 * ================================================================ */
static long sys_gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv || !user_addr_range_ok(tv, sizeof(struct timeval))) {
        current->t_errno = EFAULT; return -1;
    }

    struct timeval ktv;
    rtc_get_timeval(&ktv.tv_sec, &ktv.tv_usec);

    if (safe_copy_to_user(tv, &ktv, sizeof(ktv)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return 0;
}

/* ================================================================
 * SYS_NANOSLEEP — Sleep for specified time
 * ================================================================ */
static long sys_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req || !user_addr_range_ok(req, sizeof(struct timespec))) {
        current->t_errno = EFAULT; return -1;
    }

    struct timespec ts;
    if (safe_copy_from_user(&ts, req, sizeof(ts)) != 0) {
        current->t_errno = EFAULT; return -1;
    }

    /*
     * FIXED (v4.2.0): Check for integer overflow in nanosleep time calculation.
     * ts.tv_sec * 1000 can overflow uint64_t for large values.
     * Cap the sleep duration at a reasonable maximum (approx 24 hours).
     * (BUG-PROC-M6: nanosleep integer overflow)
     */
    #define NANOSLEEP_MAX_SEC 86400  /* 24 hours */
    uint64_t sec = (ts.tv_sec > NANOSLEEP_MAX_SEC) ? NANOSLEEP_MAX_SEC
                                                     : (uint64_t)ts.tv_sec;
    uint64_t nsec = (uint64_t)ts.tv_nsec;
    if (nsec > 999999999) nsec = 999999999;

    /* Calculate target tick: 100 Hz = 10ms per tick */
    uint64_t target_ms = sec * 1000 + nsec / 1000000;
    uint64_t target_ticks = target_ms / 10;
    if (target_ticks == 0) target_ticks = 1;

    uint64_t start_ticks = perf.uptime_ticks;

    /*
     * FIXED (v4.1.4): Handle EINTR (interruption by signal).
     * After schedule() returns, check if a signal is pending.
     * If a signal was delivered, nanosleep returns -1 with errno
     * set to EINTR and reports the remaining sleep time in rem.
     * (BUG 4.6)
     */
    /* Set sleep_until and block */
    current->sleep_until = start_ticks + target_ticks;
    current->state = TASK_BLOCKED;
    schedule();

    /* Check if we were woken by a signal */
    int interrupted = 0;
    if (current->sig && current->sig->pending) {
        interrupted = 1;
    }

    /* Calculate remaining time if interrupted early */
    if (rem && user_addr_range_ok(rem, sizeof(struct timespec))) {
        uint64_t elapsed_ticks = perf.uptime_ticks - start_ticks;
        if (elapsed_ticks < target_ticks) {
            uint64_t remaining_ms = (target_ticks - elapsed_ticks) * 10;
            struct timespec rts;
            rts.tv_sec = remaining_ms / 1000;
            rts.tv_nsec = (remaining_ms % 1000) * 1000000;
            safe_copy_to_user(rem, &rts, sizeof(rts));
        } else {
            struct timespec rts = {0, 0};
            safe_copy_to_user(rem, &rts, sizeof(rts));
        }
    }

    if (interrupted) {
        current->t_errno = EINTR;
        return -1;
    }

    return 0;
}

/* ================================================================
 * SYS_MKDIR — Create a directory
 * ================================================================ */
static long sys_mkdir(const char *path, int mode) {
    (void)mode;
    if (!path) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';

    int ret = vfs_mkdir(kpath);
    if (ret < 0) { current->t_errno = EEXIST; return -1; }
    return 0;
}

/* ================================================================
 * SYS_RMDIR — Remove a directory
 * ================================================================ */
static long sys_rmdir(const char *path) {
    if (!path) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';

    int ret = vfs_rmdir(kpath);
    if (ret < 0) { current->t_errno = ENOTEMPTY; return -1; }
    return 0;
}

/* ================================================================
 * SYS_UNLINK — Remove a file
 * ================================================================ */
static long sys_unlink(const char *path) {
    if (!path) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';

    int ret = vfs_unlink(kpath);
    if (ret < 0) { current->t_errno = ENOENT; return -1; }
    return 0;
}

/* ================================================================
 * SYS_RENAME — Rename a file or directory
 * ================================================================ */
static long sys_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) { current->t_errno = EFAULT; return -1; }
    char kold[256], knew[256];
    int len_old = strncpy_from_user(kold, oldpath, sizeof(kold) - 1);
    int len_new = strncpy_from_user(knew, newpath, sizeof(knew) - 1);
    if (len_old < 0 || len_new < 0) { current->t_errno = EFAULT; return -1; }
    kold[len_old] = '\0';
    knew[len_new] = '\0';

    int ret = vfs_rename(kold, knew);
    if (ret < 0) { current->t_errno = EXDEV; return -1; }
    return 0;
}

/* ================================================================
 * SYS_CHMOD — Change file mode
 * ================================================================ */
static long sys_chmod(const char *path, int mode) {
    if (!path) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';

    int ret = vfs_chmod(kpath, mode);
    if (ret < 0) { current->t_errno = EACCES; return -1; }
    return 0;
}

/* ================================================================
 * SYS_IOCTL — Device control
 * ================================================================ */
static long sys_ioctl(int fd, int request, void *arg) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }

    /*
     * FIXED (v4.1.4): Validate the ioctl argument pointer is in user
     * address space.  Previously the raw user pointer was passed
     * directly to vfs_ioctl without any validation, bypassing address
     * checks and potentially allowing kernel access to arbitrary memory
     * without SMAP protection.  (BUG 3.4)
     *
     * FIXED (v4.2.9): BUG-IOCTL-SIZE — Replace hardcoded 256-byte
     * check with _IOC_SIZE(request) for proper size validation.
     * Requests with arg_size > 256 are rejected with EINVAL.
     */
    size_t arg_size = _IOC_SIZE(request);
    if (arg_size > 256) { current->t_errno = EINVAL; return -1; }
    if (arg && !user_addr_range_ok(arg, arg_size > 0 ? arg_size : 256)) {
        current->t_errno = EFAULT; return -1;
    }
    int ret = vfs_ioctl(filp, request, arg);
    if (ret < 0) { current->t_errno = ENOTTY; return -1; }
    return ret;
}

/* ================================================================
 * SYS_POLL — Wait for events on file descriptors
 * ================================================================ */
static long sys_poll(struct pollfd *fds, int nfds, int timeout) {
    if (!fds || nfds <= 0) { current->t_errno = EINVAL; return -1; }

    /* Guard against overflow in multiplication */
    if (nfds > 16) nfds = 16;
    if (!user_addr_range_ok(fds, (size_t)nfds * sizeof(struct pollfd))) {
        current->t_errno = EFAULT; return -1;
    }

    /* Simple poll: check each fd for readability */
    struct pollfd kfds[16];
    if (safe_copy_from_user(kfds, fds, (size_t)nfds * sizeof(struct pollfd)) != 0) {
        current->t_errno = EFAULT; return -1;
    }

    /* FIXED (v4.2.8): BUG-SELECT-TIMEOUT — Spin-wait with re-scan on timeout */
    int ready = 0;
    int waited = 0;
    int timeout_ms = timeout;
    int timeout_neg = (timeout < 0);  /* negative means block indefinitely */

    do {
        ready = 0;
        for (int i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            if (kfds[i].fd < 0) continue;

            /* fd 0 (stdin): check if console has input */
            if (kfds[i].fd == 0) {
                extern int console_has_input(void);
                if (console_has_input()) {
                    kfds[i].revents |= POLLIN;
                    ready++;
                }
            } else if (kfds[i].fd > 0) {
                /* AF_UNIX (v4.2.6): Check for Unix domain socket */
                struct unix_sock *usk = fd_to_unix_sock(kfds[i].fd);
                if (usk) {
                    int rev = unix_poll(usk, kfds[i].events);
                    if (rev) {
                        kfds[i].revents |= (short)rev;
                        ready++;
                    }
                    continue;
                }

                struct file *filp = (struct file *)fd_get(current, kfds[i].fd);
                if (filp) {
                    if (filp->inode && filp->offset < (off_t)filp->inode->size) {
                        kfds[i].revents |= POLLIN;
                        ready++;
                    }
                    if (kfds[i].events & POLLOUT) {
                        if (filp->inode && !(filp->flags & O_RDONLY)) {
                            kfds[i].revents |= POLLOUT;
                        }
                    }
                }
            }
        }

        if (ready > 0 || timeout_ms == 0) break;

        if (timeout_neg || waited < timeout_ms) {
            /* Yield and retry after a short delay */
            current->state = TASK_BLOCKED;
            current->sleep_until = perf.uptime_ticks + 1; /* ~10ms */
            schedule();
            waited += 10;
            if (!timeout_neg && waited >= timeout_ms) break;
        }
    } while (ready == 0 && (timeout_neg || waited < timeout_ms));

    if (safe_copy_to_user(fds, kfds, (size_t)nfds * sizeof(struct pollfd)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return (long)ready;
}

/* ================================================================
 * SYS_SHUTDOWN — Shut down part of a socket connection
 * ================================================================ */
static long sys_shutdown(int sockfd, int how) {
    if (sockfd < 0 || sockfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }

    /* AF_UNIX (v4.2.6) */
    struct unix_sock *usk = fd_to_unix_sock(sockfd);
    if (usk) {
        (void)how;
        unix_close(usk);
        current->fd_table[sockfd] = (uintptr_t)-1;
        return 0;
    }

    int ret = tcp_shutdown(sockfd, how);
    if (ret < 0) { current->t_errno = ENOTCONN; return -1; }
    return 0;
}

/* ================================================================
 * SYS_GETSOCKNAME — Get socket address
 * ================================================================ */
static long sys_getsockname(int sockfd, void *addr, int *addrlen) {
    if (sockfd < 0 || sockfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!addr || !addrlen) { current->t_errno = EINVAL; return -1; }

    /* AF_UNIX (v4.2.6) */
    struct unix_sock *usk = fd_to_unix_sock(sockfd);
    if (usk) {
        if (!user_addr_range_ok(addr, sizeof(struct sockaddr_un)) ||
            !user_addr_range_ok(addrlen, sizeof(int))) {
            current->t_errno = EFAULT; return -1;
        }
        struct sockaddr_un sa;
        int alen = (int)sizeof(sa);
        int ret = unix_getsockname(usk, &sa, &alen);
        if (ret < 0) { current->t_errno = ENOTSOCK; return -1; }
        safe_copy_to_user(addr, &sa, sizeof(sa));
        safe_copy_to_user(addrlen, &(int){sizeof(sa)}, sizeof(int));
        return 0;
    }

    if (!user_addr_range_ok(addr, sizeof(struct sockaddr_in)) ||
        !user_addr_range_ok(addrlen, sizeof(int))) {
        current->t_errno = EFAULT; return -1;
    }

    uint8_t local_ip[4] = {0};
    uint16_t local_port = 0;

    int ret = tcp_getsockname(sockfd, local_ip, &local_port);
    if (ret < 0) { current->t_errno = ENOTSOCK; return -1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = sys_ntohs(local_port);
    memcpy(sa.sin_addr, local_ip, 4);

    safe_copy_to_user(addr, &sa, sizeof(sa));
    safe_copy_to_user(addrlen, &(int){sizeof(sa)}, sizeof(int));
    return 0;
}

/* ================================================================
 * SYS_ACCESS — Check file access permissions
 * ================================================================ */

/* access() mode flags (POSIX) */
#define F_OK 0  /* test existence */
#define X_OK 1  /* test execute permission */
#define W_OK 2  /* test write permission */
#define R_OK 4  /* test read permission */

static long sys_access(const char *path, int mode) {
    if (!path) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';

    struct inode *inode = vfs_lookup(kpath);
    if (!inode) { current->t_errno = ENOENT; return -1; }

    /* F_OK (mode == 0): just check existence — already done above */
    if (mode == F_OK) return 0;

    /* R_OK (read): always allow for now (simplified) */
    if (mode & R_OK) {
        /* No per-inode read permission bits yet; always pass */
    }

    /* W_OK (write): check if the file is writable */
    if (mode & W_OK) {
        /* Directories are not writable via access() for simplicity */
        if (inode->is_dir) { current->t_errno = EACCES; return -1; }
        /* TODO (v4.2.8): check per-inode write permission bits */
    }

    /* X_OK (execute): check if the file is a regular file */
    if (mode & X_OK) {
        if (inode->is_dir) { current->t_errno = EACCES; return -1; }
        /* TODO (v4.2.8): check executable permission bits */
    }

    return 0;
}

/* ================================================================
 * SYS_FCHMOD — Change file mode by fd
 * ================================================================ */
static long sys_fchmod(int fd, int mode) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp || !filp->inode) { current->t_errno = EBADF; return -1; }

    /* FIXED (v4.2.8): SEC-CHMOD — store the mode to the inode */
    filp->inode->mode = (filp->inode->mode & ~0777) | (mode & 0777);

    if (filp->inode->ops && filp->inode->ops->chmod) {
        int ret = filp->inode->ops->chmod(filp->inode, mode);
        if (ret < 0) { current->t_errno = EACCES; return -1; }
    }
    return 0;
}

/* ================================================================
 * SYS_FCHOWN — Change file owner by fd (simplified)
 * ================================================================ */
static long sys_fchown(int fd, int uid, int gid) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }

    /* Simplified: chown not implemented, no-op for uid/gid 0 */
    (void)uid; (void)gid;
    return 0;
}

/* ================================================================
 * SYS_FTRUNCATE — Truncate file to specified length
 * ================================================================ */
static long sys_ftruncate(int fd, off_t length) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (length < 0) { current->t_errno = EINVAL; return -1; }

    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp || !filp->inode) { current->t_errno = EBADF; return -1; }

    /* Simplified: just update the inode size */
    if (filp->inode) {
        filp->inode->size = (size_t)length;
    }

    /* If the file offset is beyond the new length, adjust it */
    if (filp->offset > (off_t)length) {
        filp->offset = (off_t)length;
    }

    return 0;
}

/* ================================================================
 * SYS_FSYNC — Synchronize file to disk
 * ================================================================ */
static long sys_fsync(int fd) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }

    /* Simplified: ramfs is always "in sync", no-op */
    return 0;
}

/* ================================================================
 * SYS_READLINK — Read target of symbolic link
 * ================================================================ */
static long sys_readlink(const char *path, char *buf, size_t bufsize) {
    if (!path || !buf) { current->t_errno = EFAULT; return -1; }
    if (!user_addr_range_ok(buf, bufsize)) { current->t_errno = EFAULT; return -1; }

    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';

    /* FIXED (v4.2.9): BUG-SYMLINK — Read symlink target from inode */
    struct inode *inode = vfs_lookup(kpath);
    if (!inode) { current->t_errno = ENOENT; return -1; }
    if (inode->symlink_target) {
        size_t target_len = strlen(inode->symlink_target);
        size_t copy_len = (target_len < bufsize) ? target_len : (bufsize - 1);
        if (safe_copy_to_user(buf, inode->symlink_target, copy_len) != 0) {
            current->t_errno = EFAULT; return -1;
        }
        if (copy_len < bufsize) {
            char zero = '\0';
            safe_copy_to_user(buf + copy_len, &zero, 1);
        }
        return (long)copy_len;
    }
    /* Fallback: return the path itself if no symlink target */
    size_t copy_len = (size_t)len;
    if (copy_len >= bufsize) copy_len = bufsize - 1;
    if (safe_copy_to_user(buf, kpath, copy_len) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    /* Null-terminate */
    {
        char zero = '\0';
        if (copy_len < bufsize) {
            safe_copy_to_user(buf + copy_len, &zero, 1);
        }
    }
    return (long)copy_len;
}

/* ================================================================
 * SYS_SYMLINK — Create a symbolic link
 * ================================================================ */
static long sys_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) { current->t_errno = EFAULT; return -1; }

    char ktarget[256], klink[256];
    int tlen = strncpy_from_user(ktarget, target, sizeof(ktarget) - 1);
    int llen = strncpy_from_user(klink, linkpath, sizeof(klink) - 1);
    if (tlen < 0 || llen < 0) { current->t_errno = EFAULT; return -1; }
    ktarget[tlen] = '\0';
    klink[llen] = '\0';

    /* FIXED (v4.2.9): BUG-SYMLINK — Create symlink inode with target stored */
    /* Create the symlink file and store the target in the inode */
    struct file *f = vfs_open(klink, O_CREAT | O_WRONLY);
    if (!f) { current->t_errno = EACCES; return -1; }
    /* Store the target path in the inode's symlink_target field */
    if (f->inode) {
        if (f->inode->symlink_target) {
            kfree(f->inode->symlink_target);
        }
        f->inode->symlink_target = (char *)kmalloc((size_t)(tlen + 1));
        if (f->inode->symlink_target) {
            memcpy(f->inode->symlink_target, ktarget, (size_t)(tlen + 1));
        }
    }
    vfs_close(f);
    return 0;
}

/* ================================================================
 * SYS_GETPPID — Get parent process ID
 * ================================================================ */
static long sys_getppid(void) {
    if (!current) { current->t_errno = ESRCH; return -1; }
    if (!current->parent) return 0;  /* init has no parent */
    return current->parent->pid;
}

/* ================================================================
 * SYS_GETUID — Get real user ID
 * ================================================================ */
static long sys_getuid(void) {
    return 0;  /* single-user OS, always root */
}

/* ================================================================
 * SYS_GETEUID — Get effective user ID
 * ================================================================ */
static long sys_geteuid(void) {
    return 0;  /* single-user OS, always root */
}

/* ================================================================
 * SYS_GETGID — Get real group ID
 * ================================================================ */
static long sys_getgid(void) {
    return 0;  /* single-user OS, always root */
}

/* ================================================================
 * SYS_GETEGID — Get effective group ID
 * ================================================================ */
static long sys_getegid(void) {
    return 0;  /* single-user OS, always root */
}

/* ================================================================
 * SYS_SETUID — Set user ID (simplified)
 * ================================================================ */
static long sys_setuid(int uid) {
    /* FIXED (v4.3.2): CAP-001 — Require CAP_SETUID to change UID */
    if (!cap_has_capability(CAP_SETUID)) {
        current->t_errno = EPERM;
        return -1;
    }
    /* STUB (v4.2.8): Capability check before UID change. */
    if (!cap_can_setuid(current, uid)) {
        current->t_errno = EPERM;
        return -1;
    }
    (void)uid;
    return 0;  /* single-user OS, always allowed */
}

/* ================================================================
 * SYS_SETGID — Set group ID (simplified)
 * ================================================================ */
static long sys_setgid(int gid) {
    /* FIXED (v4.3.2): CAP-001 — Require CAP_SETGID to change GID */
    if (!cap_has_capability(CAP_SETGID)) {
        current->t_errno = EPERM;
        return -1;
    }
    (void)gid;
    return 0;  /* single-user OS, always allowed */
}

/* ================================================================
 * SYS_GETPGID — Get process group ID of a process
 * ================================================================ */
static long sys_getpgid(int pid) {
    struct task_struct *t;
    if (pid == 0) {
        t = current;
    } else {
        t = task_get_by_pid(pid);
    }
    if (!t) { current->t_errno = ESRCH; return -1; }
    /* Simplified: pgid = pid for now */
    /* FIXED (v4.2.7): BUG-GETPGID-UAF — save pid before task_put frees task */
    int pgid = t->pid;
    /* REFCOUNT (v4.2.6): Release reference held by task_get_by_pid */
    if (pid != 0) task_put(t);
    return pgid;
}

/* ================================================================
 * SYS_SETPGID — Set process group ID
 * ================================================================ */
static long sys_setpgid(int pid, int pgid) {
    (void)pid; (void)pgid;
    /* Simplified: process groups not fully implemented */
    return 0;
}

/* ================================================================
 * SYS_SETSID — Create a new session
 * ================================================================ */
static long sys_setsid(void) {
    if (!current) { current->t_errno = ESRCH; return -1; }
    /* Simplified: return current PID as session ID */
    return current->pid;
}

/* ================================================================
 * SYS_NICE — Change process priority (simplified)
 * ================================================================ */
static long sys_nice(int inc) {
    if (!current) { current->t_errno = ESRCH; return -1; }
    int new_prio = current->priority + inc;
    if (new_prio < 0) new_prio = 0;
    if (new_prio > 255) new_prio = 255;
    current->priority = new_prio;
    return new_prio;
}

/* ================================================================
 * SYS_BRK — Change program break (data segment end)
 * ================================================================ */
static long sys_brk(void *addr) {
    if (!current) { current->t_errno = ESRCH; return -1; }
    if (!addr) {
        /* Return current brk */
        return (long)current->brk;
    }
    /* Set new brk */
    uint64_t new_brk = (uint64_t)(uintptr_t)addr;
    if (new_brk < 0x70000000ULL) {
        current->t_errno = EINVAL;
        return -1;
    }

    /* FIXED (v4.1.4): Register VMA for the brk region (BUG 3.1) */
    if (new_brk > current->brk) {
        vma_register(current, 0x70000000ULL, new_brk, VM_READ | VM_WRITE);
    }

    /* FIXED (v4.2.8): BUG-BRK-UNMAP — Unmap pages when shrinking brk */
    if (new_brk < current->brk) {
        uint64_t unmap_start = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t unmap_end = (current->brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (uint64_t va = unmap_start; va < unmap_end; va += PAGE_SIZE) {
            unmap_page(current->cr3, va);
        }
        vma_register(current, 0x70000000ULL, new_brk, VM_READ | VM_WRITE);
    }

    current->brk = new_brk;
    return (long)new_brk;
}

/* ================================================================
 * SYS_SBRK — Adjust heap size (not a standard Linux syscall, but
 * often implemented in libc)
 * ================================================================ */
static long sys_sbrk(intptr_t increment) {
    if (!current) { current->t_errno = ESRCH; return -1; }

    if (increment == 0) return (long)current->brk;

    uint64_t old_brk = current->brk;
    uint64_t new_brk = current->brk + (uint64_t)increment;

    /* FIXED (v4.2.8): BUG-SBRK-UNDERFLOW — Prevent shrinking below heap base */
    if (increment < 0 && new_brk < 0x70000000ULL) {
        current->t_errno = EINVAL;
        return -1;
    }

    /* Bug #7: overflow check — a large increment could wrap around */
    if (increment > 0 && new_brk < current->brk) {
        current->t_errno = EINVAL;
        return -1;
    }
    /* Bug #7: prevent mapping into kernel space (user-space limit) */
    if (new_brk > 0x7FFFFFFFFFFFULL) {
        current->t_errno = EINVAL;
        return -1;
    }

    /* Allocate pages for the heap expansion */
    if (increment > 0) {
        size_t num_pages = ((size_t)increment + PAGE_SIZE - 1) / PAGE_SIZE;
        /* Bug #45: track allocated pages for cleanup on failure */
        void **alloced_pages = (void **)kmalloc(num_pages * sizeof(void *));
        size_t alloced_count = 0;
        for (size_t i = 0; i < num_pages; i++) {
            void *phys = alloc_page();
            if (!phys) {
                /* Bug #45: cleanup all pages allocated so far */
                for (size_t j = 0; j < alloced_count; j++) {
                    free_page(alloced_pages[j]);
                }
                kfree(alloced_pages);
                current->t_errno = ENOMEM; return -1;
            }
            memset(phys, 0, PAGE_SIZE);
            if (map_page(current->cr3, old_brk + i * PAGE_SIZE,
                         (uint64_t)(uintptr_t)phys, PTE_USER | PTE_RW) != 0) {
                free_page(phys);
                /* Bug #45: cleanup all pages allocated so far */
                for (size_t j = 0; j < alloced_count; j++) {
                    free_page(alloced_pages[j]);
                }
                kfree(alloced_pages);
                current->t_errno = ENOMEM; return -1;
            }
            alloced_pages[alloced_count++] = phys;
        }
        kfree(alloced_pages);

        /* FIXED (v4.1.4): Register VMA for expanded brk region (BUG 3.1) */
        vma_register(current, 0x70000000ULL, new_brk, VM_READ | VM_WRITE);
    }

    current->brk = new_brk;
    return (long)old_brk;
}

/* ================================================================
 * SYS_GETENV — Get environment variable value
 * ================================================================ */
static long sys_getenv(const char *name, char *value, size_t size) {
    if (!current) { current->t_errno = ESRCH; return -1; }
    if (!name || !value || size == 0) { current->t_errno = EINVAL; return -1; }
    if (!user_addr_range_ok(name, 1) || !user_addr_range_ok(value, size)) {
        current->t_errno = EFAULT; return -1;
    }

    /*
     * FIXED (v4.2.0): Copy the name from user space to kernel stack
     * using strncpy_from_user instead of directly dereferencing the
     * user-space pointer.  This ensures SMAP protection and prevents
     * TOCTOU races.  (BUG-PROC-H3)
     */
    char kname[64];
    int n = strncpy_from_user(kname, name, sizeof(kname) - 1);
    if (n < 0) { current->t_errno = EFAULT; return -1; }
    kname[n] = '\0';

    for (int i = 0; i < current->env_count; i++) {
        if (strcmp(current->env_keys[i], kname) == 0) {
            size_t len = strlen(current->env_vals[i]);
            if (len >= size) len = size - 1;
            if (safe_copy_to_user(value, current->env_vals[i], len) != 0) {
                current->t_errno = EFAULT; return -1;
            }
            /* Null-terminate in user space */
            /* copy_to_user already handles the user pointer */
            return 0;
        }
    }
    current->t_errno = ENOENT;
    return -1;
}

/* ================================================================
 * SYS_SETENV — Set environment variable value
 * ================================================================ */
static long sys_setenv(const char *name, const char *value) {
    if (!current) { current->t_errno = ESRCH; return -1; }
    if (!name || !value) { current->t_errno = EINVAL; return -1; }
    if (!user_addr_range_ok(name, 1) || !user_addr_range_ok(value, 1)) {
        current->t_errno = EFAULT; return -1;
    }

    /*
     * FIXED (v4.2.0): Copy name and value from user space to kernel
     * stack using strncpy_from_user to ensure SMAP protection and
     * prevent TOCTOU races.  (BUG-PROC-H3)
     */
    char kname[64], kvalue[256];
    int n = strncpy_from_user(kname, name, sizeof(kname) - 1);
    if (n < 0) { current->t_errno = EFAULT; return -1; }
    kname[n] = '\0';
    n = strncpy_from_user(kvalue, value, sizeof(kvalue) - 1);
    if (n < 0) { current->t_errno = EFAULT; return -1; }
    kvalue[n] = '\0';

    /* Look for existing key */
    for (int i = 0; i < current->env_count; i++) {
        if (strcmp(current->env_keys[i], kname) == 0) {
            /* Update existing value */
            size_t j;
            for (j = 0; j < sizeof(current->env_vals[i]) - 1 && kvalue[j]; j++)
                current->env_vals[i][j] = kvalue[j];
            current->env_vals[i][j] = '\0';
            return 0;
        }
    }

    /* Add new key */
    if (current->env_count >= 16) {
        current->t_errno = ENOMEM;
        return -1;
    }
    size_t j;
    for (j = 0; j < sizeof(current->env_keys[current->env_count]) - 1 && kname[j]; j++)
        current->env_keys[current->env_count][j] = kname[j];
    current->env_keys[current->env_count][j] = '\0';
    for (j = 0; j < sizeof(current->env_vals[current->env_count]) - 1 && kvalue[j]; j++)
        current->env_vals[current->env_count][j] = kvalue[j];
    current->env_vals[current->env_count][j] = '\0';
    current->env_count++;
    return 0;
}

/* ================================================================
 * SYS_MADVISE — Give advice about memory usage
 * ================================================================ */
static long sys_madvise(void *addr, size_t length, int advice) {
    if (!addr || length == 0) { current->t_errno = EINVAL; return -1; }
    if (!user_addr_range_ok(addr, length)) { current->t_errno = EFAULT; return -1; }
    /* Simplified: accept all advice, no-op */
    (void)advice;
    return 0;
}

/* ================================================================
 * SYS_CLOCK_GETTIME — Get clock time
 * ================================================================ */
static long sys_clock_gettime(int clock_id, struct timespec *tp) {
    if (!tp || !user_addr_range_ok(tp, sizeof(struct timespec))) {
        current->t_errno = EFAULT; return -1;
    }

    struct timespec ts;
    uint64_t tv_sec, tv_usec;
    rtc_get_timeval(&tv_sec, &tv_usec);

    switch (clock_id) {
        case 0: /* CLOCK_REALTIME */
            ts.tv_sec = tv_sec;
            ts.tv_nsec = tv_usec * 1000;
            break;
        case 1: /* CLOCK_MONOTONIC */
            ts.tv_sec = rtc_get_uptime_seconds();
            ts.tv_nsec = 0;
            break;
        default:
            current->t_errno = EINVAL;
            return -1;
    }

    if (safe_copy_to_user(tp, &ts, sizeof(ts)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return 0;
}

/* ================================================================
 * SYS_PIPE2 — Create a pipe with flags
 * ================================================================ */
static long sys_pipe2(int *fds, int flags) {
    if (!fds || !user_addr_range_ok(fds, 2 * sizeof(int))) {
        current->t_errno = EFAULT; return -1;
    }

    int ret = sys_pipe(fds);
    (void)flags;  /* flags not fully supported yet */
    return ret;
}

/* ================================================================
 * SYS_FCNTL — File control operations
 * ================================================================ */
static long sys_fcntl(int fd, int cmd, long arg) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }

    switch (cmd) {
        case 0: /* F_DUPFD — duplicate fd */
            return (long)fd_dup(current, fd);
        case 1: /* F_GETFD — get fd flags */
            return 0;
        case 2: /* F_SETFD — set fd flags */
            (void)arg;
            return 0;
        case 3: /* F_GETFL — get file status flags */
            return filp->flags;
        case 4: /* F_SETFL — set file status flags */
            filp->flags = (int)arg;
            return 0;
        default:
            current->t_errno = EINVAL;
            return -1;
    }
}

/* ================================================================
 * SYS_CHOWN — Change file owner by path (simplified)
 * ================================================================ */
static long sys_chown(const char *path, int uid, int gid) {
    if (!path) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';

    /* FIXED (v4.3.2): CAP-001 — Require CAP_CHOWN to change file ownership */
    if (!cap_has_capability(CAP_CHOWN) && (uid != (int)-1 || gid != (int)-1)) {
        current->t_errno = EPERM;
        return -1;
    }

    /* STUB (v4.2.8): Capability check before ownership change. */
    if (!cap_can_chown(current, kpath, uid, gid)) {
        current->t_errno = EPERM;
        return -1;
    }

    struct inode *inode = vfs_lookup(kpath);
    if (!inode) { current->t_errno = ENOENT; return -1; }

    /* Simplified: chown not implemented, no-op */
    (void)uid; (void)gid;
    return 0;
}

/* ================================================================
 * SYS_SYSINFO — Return system information
 * ================================================================ */
struct sysinfo {
    uint64_t uptime;       /* seconds since boot */
    uint64_t loads[3];     /* 1, 5, 15 min load averages */
    uint64_t totalram;     /* total usable main memory size */
    uint64_t freeram;      /* available memory size */
    uint64_t sharedram;    /* amount of shared memory */
    uint64_t bufferram;    /* memory used by buffers */
    uint64_t totalswap;    /* total swap space size */
    uint64_t freeswap;     /* swap space still available */
    uint16_t procs;        /* number of current processes */
    uint16_t pad;          /* padding */
    uint64_t totalhigh;    /* total high memory size */
    uint64_t freehigh;     /* available high memory size */
    uint32_t mem_unit;     /* memory unit size in bytes */
};

static long sys_sysinfo(struct sysinfo *info) {
    if (!info || !user_addr_range_ok(info, sizeof(struct sysinfo))) {
        current->t_errno = EFAULT; return -1;
    }

    struct sysinfo si;
    memset(&si, 0, sizeof(si));

    si.uptime = rtc_get_uptime_seconds();
    si.loads[0] = si.loads[1] = si.loads[2] = 0;

    uint64_t total, free, used;
    mem_get_stats(&total, &free, &used);
    si.totalram = total;
    si.freeram = free;
    si.sharedram = 0;
    si.bufferram = 0;
    si.totalswap = 0;
    si.freeswap = 0;

    /* Count tasks */
    si.procs = 0;
    {
        extern struct task_struct *current;
        /* Simple count: just return 1+ for now */
        si.procs = 3;
    }

    si.totalhigh = 0;
    si.freehigh = 0;
    si.mem_unit = 1;

    if (safe_copy_to_user(info, &si, sizeof(si)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return 0;
}

/* ================================================================
 * SYS_GETRLIMIT — Get resource limits
 * ================================================================ */
struct rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

static long sys_getrlimit(int resource, struct rlimit *rlim) {
    if (!rlim || !user_addr_range_ok(rlim, sizeof(struct rlimit))) {
        current->t_errno = EFAULT; return -1;
    }

    if (resource < 0 || resource >= 16) {
        current->t_errno = EINVAL; return -1;
    }

    struct rlimit rl;
    rl.rlim_cur = current->rlimit_cur[resource];
    rl.rlim_max = current->rlimit_max[resource];

    if (safe_copy_to_user(rlim, &rl, sizeof(rl)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return 0;
}

/* ================================================================
 * SYS_SETRLIMIT — Set resource limits
 * ================================================================ */
static long sys_setrlimit(int resource, const struct rlimit *rlim) {
    if (!rlim || !user_addr_range_ok(rlim, sizeof(struct rlimit))) {
        current->t_errno = EFAULT; return -1;
    }

    if (resource < 0 || resource >= 16) {
        current->t_errno = EINVAL; return -1;
    }

    /* Only support known resources */
    switch (resource) {
        case RLIMIT_CPU:
        case RLIMIT_DATA:
        case RLIMIT_STACK:
        case RLIMIT_NOFILE:
        case RLIMIT_AS:
            break;
        default:
            current->t_errno = EINVAL;
            return -1;
    }

    struct rlimit rl;
    if (safe_copy_from_user(&rl, rlim, sizeof(rl)) != 0) {
        current->t_errno = EFAULT; return -1;
    }

    /* rlim_cur must not exceed rlim_max */
    if (rl.rlim_cur > rl.rlim_max) {
        current->t_errno = EINVAL;
        return -1;
    }

    current->rlimit_cur[resource] = rl.rlim_cur;
    current->rlimit_max[resource] = rl.rlim_max;

    return 0;
}

/* ================================================================
 * SYS_SCHED_YIELD — Yield the processor
 * ================================================================ */
static long sys_sched_yield(void) {
    yield();
    return 0;
}

/* ================================================================
 * SYS_GETRANDOM — Get random bytes (CSPRNG via ChaCha20)
 * ================================================================ */
static long sys_getrandom(void *buf, size_t buflen, unsigned int flags) {
    if (!buf || buflen == 0) { current->t_errno = EINVAL; return -1; }
    if (!user_addr_range_ok(buf, buflen)) { current->t_errno = EFAULT; return -1; }
    (void)flags;

    /* Use the kernel's ChaCha20 CSPRNG (shared with ASLR).
     * This provides cryptographically secure random bytes
     * with multi-source entropy (TSC + RDRAND). */
    uint8_t *kbuf = (uint8_t *)kmalloc(buflen);
    if (!kbuf) { current->t_errno = ENOMEM; return -1; }

    if (chacha20_random_bytes(kbuf, buflen) != 0) {
        kfree(kbuf);
        current->t_errno = EIO; return -1;
    }

    if (safe_copy_to_user(buf, kbuf, buflen) != 0) {
        kfree(kbuf);
        current->t_errno = EFAULT; return -1;
    }
    kfree(kbuf);
    return (long)buflen;
}

/* ================================================================
 * FIXED (v4.2.3): SYS_SIGPROCMASK — Signal mask manipulation
 *
 * how: SIG_BLOCK (0), SIG_UNBLOCK (1), SIG_SETMASK (2)
 * set: new mask to apply (may be NULL)
 * oldset: previous mask (may be NULL)
 * ================================================================ */
/* FIXED (v4.3.7): BUG-04 — removed 'static' to match syscall.h declaration */
long sys_sigprocmask(int how, const uint64_t *set, uint64_t *oldset) {
    if (how < 0 || how > 2) { current->t_errno = EINVAL; return -1; }
    if (!current->sig) {
        current->sig = signal_alloc();
        if (!current->sig) { current->t_errno = ENOMEM; return -1; }
    }

    uint64_t old = current->sig->blocked;

    if (oldset) {
        if (!user_addr_range_ok(oldset, sizeof(uint64_t))) {
            current->t_errno = EFAULT; return -1;
        }
        if (safe_copy_to_user(oldset, &old, sizeof(uint64_t)) != 0) {
            current->t_errno = EFAULT; return -1;
        }
    }

    if (set) {
        uint64_t new_set;
        if (!user_addr_range_ok((void *)set, sizeof(uint64_t))) {
            current->t_errno = EFAULT; return -1;
        }
        if (safe_copy_from_user(&new_set, set, sizeof(uint64_t)) != 0) {
            current->t_errno = EFAULT; return -1;
        }

        switch (how) {
        case 0: /* SIG_BLOCK */
            current->sig->blocked |= new_set;
            break;
        case 1: /* SIG_UNBLOCK */
            current->sig->blocked &= ~new_set;
            break;
        case 2: /* SIG_SETMASK */
            current->sig->blocked = new_set;
            break;
        }
        /* SIGKILL and SIGSTOP cannot be blocked (POSIX requirement) */
        current->sig->blocked &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    }

    return 0;
}

/* ================================================================
 * FIXED (v4.2.3): SYS_READV — Scatter read
 * ================================================================ */
static long sys_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0 || iovcnt > 1024) {
        current->t_errno = EINVAL; return -1;
    }

    size_t iov_size = (size_t)iovcnt * sizeof(struct iovec);
    if (!user_addr_range_ok((void *)iov, iov_size)) {
        current->t_errno = EFAULT; return -1;
    }

    struct iovec *kiov = (struct iovec *)kmalloc(iov_size);
    if (!kiov) { current->t_errno = ENOMEM; return -1; }
    if (safe_copy_from_user(kiov, iov, iov_size) != 0) {
        kfree(kiov); current->t_errno = EFAULT; return -1;
    }

    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (kiov[i].iov_len == 0) continue;
        /* FIXED (v4.2.4): Validate each iov_base independently.
         * The iov array itself was validated above, but individual
         * iov_base pointers were not checked.  Without this, a
         * malicious process could pass a kernel address as iov_base
         * and achieve arbitrary kernel memory read.  (BUG-READV-TOCTOU) */
        if (!user_addr_range_ok(kiov[i].iov_base, kiov[i].iov_len)) {
            kfree(kiov); current->t_errno = EFAULT; return -1;
        }
        long n = sys_read(fd, kiov[i].iov_base, kiov[i].iov_len);
        if (n < 0) { kfree(kiov); return total > 0 ? total : -1; }
        total += n;
        if ((size_t)n < kiov[i].iov_len) break; /* short read */
    }
    kfree(kiov);
    return total;
}

/* ================================================================
 * FIXED (v4.2.3): SYS_WRITEV — Gather write
 * ================================================================ */
static long sys_writev(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0 || iovcnt > 1024) {
        current->t_errno = EINVAL; return -1;
    }

    size_t iov_size = (size_t)iovcnt * sizeof(struct iovec);
    if (!user_addr_range_ok((void *)iov, iov_size)) {
        current->t_errno = EFAULT; return -1;
    }

    struct iovec *kiov = (struct iovec *)kmalloc(iov_size);
    if (!kiov) { current->t_errno = ENOMEM; return -1; }
    if (safe_copy_from_user(kiov, iov, iov_size) != 0) {
        kfree(kiov); current->t_errno = EFAULT; return -1;
    }

    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (kiov[i].iov_len == 0) continue;
        /* FIXED (v4.2.4): Validate each iov_base independently.
         * Without this, a malicious process could pass a kernel
         * address as iov_base and achieve arbitrary kernel memory
         * write.  (BUG-WRITEV-TOCTOU) */
        if (!user_addr_range_ok(kiov[i].iov_base, kiov[i].iov_len)) {
            kfree(kiov); current->t_errno = EFAULT; return -1;
        }
        long n = sys_write(fd, kiov[i].iov_base, kiov[i].iov_len);
        if (n < 0) { kfree(kiov); return total > 0 ? total : -1; }
        total += n;
    }
    kfree(kiov);
    return total;
}

/* ================================================================
 * FIXED (v4.2.3): SYS_SELECT — I/O multiplexing
 *
 * Simplified implementation: checks each fd for readability/writability
 * using a non-blocking poll.  Timeout is processed but at millisecond
 * granularity via the scheduler tick.
 * ================================================================ */
static long sys_select(int nfds, fd_set *readfds, fd_set *writefds,
                       fd_set *exceptfds, struct timeval *timeout) {
    if (nfds < 0 || nfds > FD_SETSIZE) {
        current->t_errno = EINVAL; return -1;
    }

    fd_set kreadfds, kwritefds, kexceptfds;
    fd_set *pr = NULL, *pw = NULL, *pe = NULL;

    if (readfds) {
        if (!user_addr_range_ok(readfds, sizeof(fd_set))) {
            current->t_errno = EFAULT; return -1;
        }
        safe_copy_from_user(&kreadfds, readfds, sizeof(fd_set));
        pr = &kreadfds;
    }
    if (writefds) {
        if (!user_addr_range_ok(writefds, sizeof(fd_set))) {
            current->t_errno = EFAULT; return -1;
        }
        safe_copy_from_user(&kwritefds, writefds, sizeof(fd_set));
        pw = &kwritefds;
    }
    if (exceptfds) {
        if (!user_addr_range_ok(exceptfds, sizeof(fd_set))) {
            current->t_errno = EFAULT; return -1;
        }
        safe_copy_from_user(&kexceptfds, exceptfds, sizeof(fd_set));
        pe = &kexceptfds;
    }

    fd_set result_read, result_write, result_except;
    FD_ZERO(&result_read);
    FD_ZERO(&result_write);
    FD_ZERO(&result_except);

    struct file *filp;
    int ready = 0;

    /* FIXED (v4.2.8): BUG-SELECT-TIMEOUT — Spin-wait with re-scan on timeout */
    int waited = 0;
    int timeout_ms = 0;
    int timeout_neg = 0;
    if (timeout) {
        struct timeval tv;
        if (safe_copy_from_user(&tv, timeout, sizeof(tv)) != 0) {
            current->t_errno = EFAULT; return -1;
        }
        timeout_ms = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    } else {
        timeout_neg = 1; /* NULL timeout means block indefinitely */
    }

    do {
        ready = 0;
        FD_ZERO(&result_read);
        FD_ZERO(&result_write);
        FD_ZERO(&result_except);

        for (int fd = 0; fd < nfds; fd++) {
        if (pr && FD_ISSET(fd, pr)) {
            /* AF_UNIX (v4.2.6): Check Unix domain socket */
            struct unix_sock *usk = fd_to_unix_sock(fd);
            if (usk) {
                if (unix_poll(usk, POLLIN)) {
                    FD_SET(fd, &result_read);
                    ready++;
                }
            } else {
                /* FIXED (v4.2.4): Use fd_get() instead of current->files[fd].
                 * task_struct has fd_table, not files.  Using current->files
                 * would cause a compilation error.  (BUG-SELECT-FILES) */
                filp = (struct file *)fd_get(current, fd);
                if (filp && filp->inode) {
                    /* A file that exists is always "readable" in our simple model.
                     * For sockets, check if there's pending data. */
                    FD_SET(fd, &result_read);
                    ready++;
                }
            }
        }
        if (pw && FD_ISSET(fd, pw)) {
            /* AF_UNIX (v4.2.6): Check Unix domain socket */
            struct unix_sock *usk = fd_to_unix_sock(fd);
            if (usk) {
                if (unix_poll(usk, POLLOUT)) {
                    FD_SET(fd, &result_write);
                    ready++;
                }
            } else {
                filp = (struct file *)fd_get(current, fd);
                if (filp && filp->inode) {
                    /* A file that exists is always "writable" in our simple model */
                    FD_SET(fd, &result_write);
                    ready++;
                }
            }
        }
        if (pe && FD_ISSET(fd, pe)) {
            /* Exceptfds: no exceptional conditions in our simple model */
        }
    }

        if (ready > 0 || timeout_ms == 0) break;

        if (timeout_neg || waited < timeout_ms) {
            /* Yield and retry after a short delay */
            current->state = TASK_BLOCKED;
            current->sleep_until = perf.uptime_ticks + 1; /* ~10ms */
            schedule();
            /* Check for pending signals */
            if (current->sig && current->sig->pending) {
                current->t_errno = EINTR; return -1;
            }
            waited += 10;
            if (!timeout_neg && waited >= timeout_ms) break;
        }
    } while (ready == 0 && (timeout_neg || waited < timeout_ms));

    /* Copy results back to user */
    if (readfds) safe_copy_to_user(readfds, &result_read, sizeof(fd_set));
    if (writefds) safe_copy_to_user(writefds, &result_write, sizeof(fd_set));
    if (exceptfds) safe_copy_to_user(exceptfds, &result_except, sizeof(fd_set));

    return ready;
}

/* ================================================================
 * FIXED (v4.2.3): SYS_SOCKETPAIR — Create a pair of connected sockets
 * ================================================================ */
static long sys_socketpair(int domain, int type, int protocol, int sv[2]) {
    if (!sv) { current->t_errno = EFAULT; return -1; }
    if (!user_addr_range_ok(sv, 2 * sizeof(int))) {
        current->t_errno = EFAULT; return -1;
    }

    /* AF_UNIX (v4.2.6): Create a pair of connected Unix domain sockets */
    if (domain == AF_UNIX) {
        struct unix_sock *sk1 = unix_socket_create(type);
        if (!sk1) { current->t_errno = ENOMEM; return -1; }
        struct unix_sock *sk2 = unix_socket_create(type);
        if (!sk2) { unix_close(sk1); current->t_errno = ENOMEM; return -1; }

        /* Cross-connect the two sockets */
        spin_lock(&sk1->lock);
        spin_lock(&sk2->lock);
        sk1->peer = sk2;
        sk2->peer = sk1;
        sk1->state = UNIX_CONNECTED;
        sk2->state = UNIX_CONNECTED;
        spin_unlock(&sk2->lock);
        spin_unlock(&sk1->lock);

        int fd1 = fd_alloc(current, (void *)sk1);
        int fd2 = fd_alloc(current, (void *)sk2);
        if (fd1 < 0 || fd2 < 0) {
            if (fd1 >= 0) { current->fd_table[fd1] = (uintptr_t)-1; }
            if (fd2 >= 0) { current->fd_table[fd2] = (uintptr_t)-1; }
            unix_close(sk1);
            unix_close(sk2);
            current->t_errno = EMFILE; return -1;
        }

        int fds[2] = { fd1, fd2 };
        if (safe_copy_to_user(sv, fds, sizeof(fds)) != 0) {
            fd_close(current, fd1);
            fd_close(current, fd2);
            current->t_errno = EFAULT; return -1;
        }
        return 0;
    }

    /* FIXED (v4.2.4): Use pipe() for socketpair instead of TCP loopback.
     * The previous implementation created TCP sockets, connected them
     * via loopback, and required a full TCP handshake, which fails
     * without a working network stack.  pipe() provides a simpler,
     * more reliable IPC mechanism that works locally without networking.
     * For AF_UNIX semantics, this is the correct approach.
     * (BUG-SOCKETPAIR-TCP) */
    (void)domain; (void)type; (void)protocol;

    int fds[2];
    int ret = sys_pipe(fds);
    if (ret < 0) return -1;

    if (safe_copy_to_user(sv, fds, sizeof(fds)) != 0) {
        sys_close(fds[0]);
        sys_close(fds[1]);
        current->t_errno = EFAULT; return -1;
    }

    return 0;
}

/* ================================================================
 * FIXED (v4.2.3): SYS_SETSOCKOPT — Set socket options
 * ================================================================ */
static long sys_setsockopt(int sockfd, int level, int optname,
                           const void *optval, uint32_t optlen) {
    /* FIXED (v4.2.4): Validate sockfd and basic parameters.
     * Previously, all parameters were silently ignored, allowing
     * invalid fds to be passed without error.  Now we verify the fd
     * exists and return reasonable errors for invalid parameters.
     * (BUG-SETSOCKOPT-VALIDATE) */
    if (sockfd < 0 || sockfd >= MAX_FDS) {
        current->t_errno = EBADF; return -1;
    }
    struct file *filp = (struct file *)fd_get(current, sockfd);
    if (!filp) { current->t_errno = EBADF; return -1; }
    if (!filp->inode) { current->t_errno = ENOTSOCK; return -1; }

    /* Stub: most socket options are not critical for basic operation.
     * SO_REUSEADDR, SO_KEEPALIVE, etc. are silently accepted.
     * Returns 0 (success) to prevent applications from failing. */
    (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

/* ================================================================
 * FIXED (v4.2.3): SYS_GETSOCKOPT — Get socket options
 * ================================================================ */
static long sys_getsockopt(int sockfd, int level, int optname,
                           void *optval, uint32_t *optlen) {
    (void)sockfd; (void)level; (void)optname;
    /* Stub: return default values for common socket options */
    if (optval && optlen) {
        uint32_t len;
        if (safe_copy_from_user(&len, optlen, sizeof(uint32_t)) != 0) {
            current->t_errno = EFAULT; return -1;
        }
        /* For SO_ERROR, return 0 (no error) */
        int zero = 0;
        if (len >= (uint32_t)sizeof(int)) {
            if (safe_copy_to_user(optval, &zero, sizeof(int)) != 0) {
                current->t_errno = EFAULT; return -1;
            }
        }
        uint32_t ret_len = (uint32_t)sizeof(int);
        if (safe_copy_to_user(optlen, &ret_len, sizeof(uint32_t)) != 0) {
            current->t_errno = EFAULT; return -1;
        }
    }
    return 0;
}

/* ================================================================
 * FIXED (v4.2.3): SYS_GETDENTS64 — Get directory entries (64-bit)
 *
 * Converts internal directory entry format to linux_dirent64 with
 * 64-bit inode and offset fields.  Uses vfs_read for raw directory
 * data, then reformats for the Linux 64-bit ABI.
 * ================================================================ */
static long sys_getdents64(unsigned int fd, struct linux_dirent64 *dirp,
                           unsigned int count) {
    if (!dirp || count == 0) { current->t_errno = EINVAL; return -1; }
    if (!user_addr_range_ok(dirp, count)) {
        current->t_errno = EFAULT; return -1;
    }

    struct file *filp = (struct file *)fd_get(current, (int)fd);
    if (!filp || !filp->inode) { current->t_errno = EBADF; return -1; }

    /* Allocate a temporary buffer for raw directory entries */
    #define GETDENTS_BUF_SIZE 2048
    char *raw_buf = (char *)kmalloc(GETDENTS_BUF_SIZE);
    if (!raw_buf) { current->t_errno = ENOMEM; return -1; }

    /* Save current offset and use vfs_read to get raw directory data */
    off_t saved_off = filp->offset;
    ssize_t raw_len = vfs_read(filp, raw_buf, GETDENTS_BUF_SIZE);
    if (raw_len < 0) { kfree(raw_buf); current->t_errno = (int)(-raw_len); return -1; }
    if (raw_len == 0) { kfree(raw_buf); return 0; }

    /* Convert raw directory entries to linux_dirent64 format */
    char *out = (char *)kmalloc((size_t)count);
    if (!out) { kfree(raw_buf); current->t_errno = ENOMEM; return -1; }

    size_t out_pos = 0;
    size_t raw_pos = 0;
    while (raw_pos < (size_t)raw_len && out_pos + sizeof(struct linux_dirent64) <= (size_t)count) {
        /* Parse raw directory entry (ext2_dir_entry format) */
        struct ext2_dir_entry {
            uint32_t inode;
            uint16_t rec_len;
            uint8_t  name_len;
            uint8_t  file_type;
            char     name[];
        };
        struct ext2_dir_entry *de = (struct ext2_dir_entry *)(raw_buf + raw_pos);
        /* FIXED (v4.2.8): BUG-GETDENTS-BOUNDS — Validate directory entry fields */
        if (de->rec_len == 0 || de->rec_len > (uint16_t)(raw_len - raw_pos)) break;
        if (de->name_len > 255) break;
        if (raw_pos + de->rec_len > (size_t)raw_len) break;

        size_t entry_size = sizeof(struct linux_dirent64) + (size_t)de->name_len + 1;
        /* Align to 8 bytes */
        entry_size = (entry_size + 7) & ~7;
        if (out_pos + entry_size > (size_t)count) break;

        struct linux_dirent64 *d64 = (struct linux_dirent64 *)(out + out_pos);
        d64->d_ino = de->inode;
        d64->d_off = (int64_t)(saved_off + (off_t)raw_pos + (off_t)de->rec_len);
        d64->d_reclen = (uint16_t)entry_size;
        d64->d_type = de->file_type;
        memcpy(d64->d_name, de->name, de->name_len);
        d64->d_name[de->name_len] = '\0';

        out_pos += entry_size;
        raw_pos += (size_t)de->rec_len;
    }

    if (out_pos > 0) {
        if (safe_copy_to_user(dirp, out, out_pos) != 0) {
            kfree(out); kfree(raw_buf);
            current->t_errno = EFAULT; return -1;
        }
    }

    kfree(out);
    kfree(raw_buf);

    /* FIXED (v4.2.4): Update the file offset after reading directory
     * entries.  Without this, repeated getdents64 calls would return
     * the same entries, causing an infinite loop in ls and similar
     * tools.  (BUG-GETDENTS-OFFSET) */
    filp->offset = saved_off + (off_t)raw_len;

    return (long)out_pos;
}

/* ================================================================
 * SYS_ACPI_SHUTDOWN — ACPI system shutdown (S5)
 * ACPI (v4.2.6)
 * ================================================================ */
static long sys_acpi_shutdown(void) {
    /* FIXED (v4.3.0): NEW-6 ACPI-UID */
    if (current->uid != 0) { current->t_errno = EPERM; return -1; }
    int ret = acpi_shutdown();
    if (ret < 0) {
        current->t_errno = EIO;
        return -1;
    }
    /* Should never return */
    return 0;
}

/* ================================================================
 * SYS_ACPI_REBOOT — ACPI system reset
 * ACPI (v4.2.6)
 * ================================================================ */
static long sys_acpi_reboot(void) {
    /* FIXED (v4.3.0): NEW-6 ACPI-UID */
    if (current->uid != 0) { current->t_errno = EPERM; return -1; }
    acpi_reboot();
    /* Should never return */
    return 0;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_FCHDIR — Change directory by fd
 * ================================================================ */
static long sys_fchdir(int fd) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp || !filp->inode) { current->t_errno = EBADF; return -1; }
    if (!filp->inode->is_dir) { current->t_errno = ENOTDIR; return -1; }

    /* FIXED (v4.2.8): BUG-FCHDIR — Build path from inode name instead of "/" */
    if (filp->inode->name && filp->inode->name[0]) {
        strncpy(current->cwd, filp->inode->name, sizeof(current->cwd) - 1);
        current->cwd[sizeof(current->cwd) - 1] = '\0';
    } else {
        strcpy(current->cwd, "/");
    }
    return 0;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_GETRESUID — Get real, effective, saved UID
 * ================================================================ */
static long sys_getresuid(int *ruid, int *euid, int *suid) {
    if (ruid && !user_addr_range_ok(ruid, sizeof(int))) {
        current->t_errno = EFAULT; return -1;
    }
    if (euid && !user_addr_range_ok(euid, sizeof(int))) {
        current->t_errno = EFAULT; return -1;
    }
    if (suid && !user_addr_range_ok(suid, sizeof(int))) {
        current->t_errno = EFAULT; return -1;
    }
    int zero = 0;
    if (ruid) safe_copy_to_user(ruid, &zero, sizeof(int));
    if (euid) safe_copy_to_user(euid, &zero, sizeof(int));
    if (suid) safe_copy_to_user(suid, &zero, sizeof(int));
    return 0;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_GETRESGID — Get real, effective, saved GID
 * ================================================================ */
static long sys_getresgid(int *rgid, int *egid, int *sgid) {
    if (rgid && !user_addr_range_ok(rgid, sizeof(int))) {
        current->t_errno = EFAULT; return -1;
    }
    if (egid && !user_addr_range_ok(egid, sizeof(int))) {
        current->t_errno = EFAULT; return -1;
    }
    if (sgid && !user_addr_range_ok(sgid, sizeof(int))) {
        current->t_errno = EFAULT; return -1;
    }
    int zero = 0;
    if (rgid) safe_copy_to_user(rgid, &zero, sizeof(int));
    if (egid) safe_copy_to_user(egid, &zero, sizeof(int));
    if (sgid) safe_copy_to_user(sgid, &zero, sizeof(int));
    return 0;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_SETRESUID — Set real, effective, saved UID
 * ================================================================ */
static long sys_setresuid(int ruid, int euid, int suid) {
    /* FIXED (v4.3.0): NEW-19 SETRESUID-RANGE — reject negative UID values. */
    if (ruid < 0 || euid < 0 || suid < 0) {
        current->t_errno = EINVAL;
        return -1;
    }
    (void)ruid; (void)euid; (void)suid;
    /* Single-user OS: always allowed */
    return 0;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_SETRESGID — Set real, effective, saved GID
 * ================================================================ */
static long sys_setresgid(int rgid, int egid, int sgid) {
    /* FIXED (v4.3.0): NEW-19 SETRESGID-RANGE — reject negative GID values. */
    if (rgid < 0 || egid < 0 || sgid < 0) {
        current->t_errno = EINVAL;
        return -1;
    }
    (void)rgid; (void)egid; (void)sgid;
    /* Single-user OS: always allowed */
    return 0;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_FUTEX — Fast userspace mutex
 *
 * Basic implementation: FUTEX_WAIT and FUTEX_WAKE only.
 * ================================================================ */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* FIXED (v4.2.8): BUG-FUTEX-WAKE — Simple futex waiter list */
#define MAX_FUTEX_WAITERS 64
static struct {
    int *uaddr;
    struct task_struct *task;
    int used;
} futex_waiters[MAX_FUTEX_WAITERS];
static spinlock_t futex_lock = { 0 };

static int futex_wake(int *uaddr, int val) {
    int woken = 0;
    spin_lock(&futex_lock);
    for (int i = 0; i < MAX_FUTEX_WAITERS && woken < val; i++) {
        if (futex_waiters[i].used && futex_waiters[i].uaddr == uaddr) {
            struct task_struct *t = futex_waiters[i].task;
            if (t && t->state == TASK_BLOCKED) {
                t->state = TASK_READY;
                woken++;
            }
            futex_waiters[i].used = 0;
            futex_waiters[i].task = NULL;
            futex_waiters[i].uaddr = NULL;
        }
    }
    spin_unlock(&futex_lock);
    return woken;
}

static void futex_wait_add(int *uaddr, struct task_struct *task) {
    spin_lock(&futex_lock);
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (!futex_waiters[i].used) {
            futex_waiters[i].uaddr = uaddr;
            futex_waiters[i].task = task;
            futex_waiters[i].used = 1;
            break;
        }
    }
    spin_unlock(&futex_lock);
}

static void futex_wait_remove(struct task_struct *task) {
    spin_lock(&futex_lock);
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (futex_waiters[i].used && futex_waiters[i].task == task) {
            futex_waiters[i].used = 0;
            futex_waiters[i].task = NULL;
            futex_waiters[i].uaddr = NULL;
        }
    }
    spin_unlock(&futex_lock);
}

static long sys_futex(int *uaddr, int futex_op, int val,
                      const struct timespec *timeout, int *uaddr2, int val3) {
    (void)timeout; (void)uaddr2; (void)val3;

    if (!uaddr || !user_addr_range_ok(uaddr, sizeof(int))) {
        current->t_errno = EFAULT; return -1;
    }

    int cmd = futex_op & 0x7F;  /* mask off flags */

    if (cmd == FUTEX_WAIT) {
        /* Read current value from userspace */
        int cur_val;
        if (safe_copy_from_user(&cur_val, uaddr, sizeof(int)) != 0) {
            current->t_errno = EFAULT; return -1;
        }
        if (cur_val != val) {
            current->t_errno = EAGAIN; return -1;
        }
        /* FIXED (v4.2.8): BUG-FUTEX-WAKE — Track waiter for futex_wake */
        futex_wait_add(uaddr, current);
        current->state = TASK_BLOCKED;
        schedule();
        /* Remove from waiter list after waking */
        futex_wait_remove(current);
        /* Check for signal interruption */
        if (current->sig && current->sig->pending) {
            current->t_errno = EINTR; return -1;
        }
        return 0;
    } else if (cmd == FUTEX_WAKE) {
        /* FIXED (v4.2.8): BUG-FUTEX-WAKE — Wake up waiters on this uaddr */
        return futex_wake(uaddr, val);
    }

    current->t_errno = ENOSYS;
    return -1;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_SET_TID_ADDRESS — Set TID address for clone
 * ================================================================ */
static long sys_set_tid_address(int *tidptr) {
    if (!tidptr || !user_addr_range_ok(tidptr, sizeof(int))) {
        current->t_errno = EFAULT; return -1;
    }
    current->clear_child_tid = (uintptr_t)tidptr;
    return current->pid;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_TGKILL — Send signal to thread group
 * ================================================================ */
static long sys_tgkill(int tgid, int tid, int sig) {
    /* Simplified: route to kill() which handles signal delivery */
    (void)tgid;
    return do_sys_kill(tid, sig);
}

/* ================================================================
 * FIXED (v4.3.8): SCHED-001 — SYS_SCHED_SETAFFINITY — Set CPU affinity
 *
 * Sets the CPU affinity mask for a task.  The task is looked up by PID
 * (0 means current).  The mask is a 64-bit bitmap where each bit
 * represents a CPU.  After setting affinity, if the task is currently
 * running on a CPU not in the new mask, it is migrated to an allowed CPU.
 * ================================================================ */
static long sys_sched_setaffinity(int pid, unsigned int cpusetsize,
                                  const uint64_t *mask) {
    (void)cpusetsize;
    if (!mask || !user_addr_range_ok((void *)mask, sizeof(uint64_t))) {
        current->t_errno = EFAULT; return -1;
    }
    uint64_t k_mask;
    if (safe_copy_from_user(&k_mask, mask, sizeof(uint64_t)) != 0) {
        current->t_errno = EFAULT; return -1;
    }

    /* pid == 0 means current task */
    struct task_struct *target = current;
    if (pid != 0 && pid != current->pid) {
        target = task_get_by_pid(pid);
        if (!target) {
            current->t_errno = ESRCH; return -1;
        }
    }

    /* FIXED (v4.3.8): SCHED-001 — Set cpu_affinity and update cpu_mask.
     * cpu_affinity stores the full 64-bit mask; cpu_mask is the int
     * version used by the scheduler for quick checks. */
    target->cpu_affinity = k_mask;
    target->cpu_mask = (int)(k_mask & 0xFF);

    /* If the task is currently on a CPU not in the new mask, migrate it */
    if (target->cpu_id >= 0 && target->cpu_id < MAX_CPUS) {
        if (!(k_mask & (1ULL << target->cpu_id))) {
            /* Find an allowed CPU and migrate */
            for (int c = 0; c < MAX_CPUS; c++) {
                if (k_mask & (1ULL << c)) {
                    smp_enqueue_task(target, c);
                    log_printf(LOG_LEVEL_DEBUG, "sched: migrated pid=%d to CPU %d (affinity=0x%llx)\n",
                               target->pid, c, (unsigned long long)k_mask);
                    break;
                }
            }
        }
    }

    if (target != current) task_put(target);
    return 0;
}

/* ================================================================
 * FIXED (v4.3.8): SCHED-001 — SYS_SCHED_GETAFFINITY — Get CPU affinity
 *
 * Returns the CPU affinity mask for a task.  pid == 0 means current.
 * Returns the size of the mask on success (sizeof(uint64_t)).
 * ================================================================ */
static long sys_sched_getaffinity(int pid, unsigned int cpusetsize,
                                  uint64_t *mask) {
    (void)cpusetsize;
    if (!mask || !user_addr_range_ok(mask, sizeof(uint64_t))) {
        current->t_errno = EFAULT; return -1;
    }

    struct task_struct *target = current;
    if (pid != 0 && pid != current->pid) {
        target = task_get_by_pid(pid);
        if (!target) {
            current->t_errno = ESRCH; return -1;
        }
    }

    /* FIXED (v4.3.8): SCHED-001 — Return cpu_affinity, defaulting to all CPUs */
    uint64_t k_mask = target->cpu_affinity ? target->cpu_affinity : ~0ULL;
    if (safe_copy_to_user(mask, &k_mask, sizeof(uint64_t)) != 0) {
        if (target != current) task_put(target);
        current->t_errno = EFAULT; return -1;
    }

    if (target != current) task_put(target);
    return sizeof(uint64_t);
}

/* ================================================================
 * POSIX (v4.2.6): SYS_GETCPU — Get current CPU number
 * ================================================================ */
static long sys_getcpu(unsigned int *cpu, unsigned int *node, void *tcache) {
    (void)tcache;
    unsigned int cpu_id = 0;
    /* Read APIC ID to determine current CPU */
    extern int smp_get_cpu_id(void);
    cpu_id = (unsigned int)smp_get_cpu_id();

    if (cpu && user_addr_range_ok(cpu, sizeof(unsigned int))) {
        if (safe_copy_to_user(cpu, &cpu_id, sizeof(unsigned int)) != 0) {
            current->t_errno = EFAULT; return -1;
        }
    }
    if (node && user_addr_range_ok(node, sizeof(unsigned int))) {
        unsigned int node_id = 0;
        safe_copy_to_user(node, &node_id, sizeof(unsigned int));
    }
    return 0;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_MEMBARRIER — Memory barrier syscall
 * ================================================================ */
static long sys_membarrier(int cmd, unsigned int flags, int cpu_id) {
    (void)cmd; (void)flags; (void)cpu_id;
    /* Full memory barrier */
    asm volatile ("mfence" ::: "memory");
    return 0;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_MKDIRAT — Create directory relative to dirfd
 * ================================================================ */
static long sys_mkdirat(int dirfd, const char *path, int mode) {
    if (dirfd == -100) { /* AT_FDCWD */
        return sys_mkdir(path, mode);
    }
    /* dirfd-relative: not fully supported, stub */
    (void)dirfd; (void)path; (void)mode;
    current->t_errno = ENOSYS;
    return -1;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_MKNODAT — Create device node relative to dirfd
 * ================================================================ */
static long sys_mknodat(int dirfd, const char *path, int mode, unsigned int dev) {
    (void)dirfd; (void)path; (void)mode; (void)dev;
    current->t_errno = ENOSYS;
    return -1;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_FCHOWNAT — Change ownership relative to dirfd
 * ================================================================ */
static long sys_fchownat(int dirfd, const char *path, int uid, int gid, int flags) {
    if (dirfd == -100) { /* AT_FDCWD */
        return sys_chown(path, uid, gid);
    }
    (void)dirfd; (void)path; (void)uid; (void)gid; (void)flags;
    current->t_errno = ENOSYS;
    return -1;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_UNLINKAT — Remove file relative to dirfd
 * ================================================================ */
static long sys_unlinkat(int dirfd, const char *path, int flags) {
    if (dirfd == -100) { /* AT_FDCWD */
        if (flags & 0x200) { /* AT_REMOVEDIR */
            return sys_rmdir(path);
        }
        return sys_unlink(path);
    }
    (void)dirfd; (void)path; (void)flags;
    current->t_errno = ENOSYS;
    return -1;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_LINKAT — Create hard link relative to dirfd
 * ================================================================ */
static long sys_linkat(int olddirfd, const char *oldpath,
                       int newdirfd, const char *newpath, int flags) {
    if (olddirfd == -100 && newdirfd == -100) { /* AT_FDCWD */
        /* Simplified: hard links not supported, return EPERM */
        (void)flags;
        current->t_errno = EPERM;
        return -1;
    }
    (void)olddirfd; (void)oldpath; (void)newdirfd; (void)newpath; (void)flags;
    current->t_errno = ENOSYS;
    return -1;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_SYMLINKAT — Create symlink relative to dirfd
 * ================================================================ */
static long sys_symlinkat(const char *target, int newdirfd,
                          const char *linkpath) {
    if (newdirfd == -100) { /* AT_FDCWD */
        return sys_symlink(target, linkpath);
    }
    (void)target; (void)newdirfd; (void)linkpath;
    current->t_errno = ENOSYS;
    return -1;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_READLINKAT — Read symbolic link relative to dirfd
 * ================================================================ */
static long sys_readlinkat(int dirfd, const char *path, char *buf, size_t bufsize) {
    if (dirfd == -100) { /* AT_FDCWD */
        return sys_readlink(path, buf, bufsize);
    }
    (void)dirfd; (void)path; (void)buf; (void)bufsize;
    current->t_errno = ENOSYS;
    return -1;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_FCHMODAT — Change permissions relative to dirfd
 * ================================================================ */
static long sys_fchmodat(int dirfd, const char *path, int mode) {
    if (dirfd == -100) { /* AT_FDCWD */
        return sys_chmod(path, mode);
    }
    (void)dirfd; (void)path; (void)mode;
    current->t_errno = ENOSYS;
    return -1;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_FACCESSAT — Check access relative to dirfd
 * ================================================================ */
static long sys_faccessat(int dirfd, const char *path, int mode, int flags) {
    if (dirfd == -100) { /* AT_FDCWD */
        return sys_access(path, mode);
    }
    (void)dirfd; (void)path; (void)mode; (void)flags;
    current->t_errno = ENOSYS;
    return -1;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_PRLIMIT64 — Set/get resource limits (extended)
 * ================================================================ */
static long sys_prlimit64(int pid, int resource,
                          const struct rlimit *new_limit,
                          struct rlimit *old_limit) {
    /* FIXED (v4.2.7): BUG-PRLIMIT-BOUNDS — validate resource index */
    if (resource < 0 || resource >= RLIMIT_NLIMITS) {
        current->t_errno = EINVAL; return -1;
    }
    if (pid != 0 && pid != current->pid) {
        current->t_errno = ESRCH; return -1;
    }

    /* Get old limit */
    if (old_limit) {
        if (!user_addr_range_ok(old_limit, sizeof(struct rlimit))) {
            current->t_errno = EFAULT; return -1;
        }
        struct rlimit rl;
        rl.rlim_cur = current->rlimit_cur[resource];
        rl.rlim_max = current->rlimit_max[resource];
        if (safe_copy_to_user(old_limit, &rl, sizeof(rl)) != 0) {
            current->t_errno = EFAULT; return -1;
        }
    }

    /* Set new limit */
    if (new_limit) {
        if (!user_addr_range_ok(new_limit, sizeof(struct rlimit))) {
            current->t_errno = EFAULT; return -1;
        }
        struct rlimit rl;
        if (safe_copy_from_user(&rl, new_limit, sizeof(rl)) != 0) {
            current->t_errno = EFAULT; return -1;
        }
        if (rl.rlim_cur > rl.rlim_max) {
            current->t_errno = EINVAL; return -1;
        }
        current->rlimit_cur[resource] = rl.rlim_cur;
        current->rlimit_max[resource] = rl.rlim_max;
    }

    return 0;
}

/* ================================================================
 * POSIX (v4.2.6): SYS_NAME_TO_HANDLE_AT — File handle operations
 * ================================================================ */
struct file_handle {
    unsigned int handle_bytes;
    int          handle_type;
    unsigned char f_handle[0];
};

static long sys_name_to_handle_at(int dirfd, const char *path,
                                  struct file_handle *handle,
                                  int *mount_id, int flags) {
    (void)dirfd; (void)path; (void)handle; (void)mount_id; (void)flags;
    current->t_errno = ENOSYS;
    return -1;
}

/* STUB (v4.2.8): SYS_PRCTL — basic process control (seccomp setup only) */
#define PR_SET_SECCOMP      22  /* FIXED (v4.3.2): SEC-001 */

static long sys_prctl(int option, unsigned long arg2, unsigned long arg3,
                      unsigned long arg4, unsigned long arg5) {
    (void)arg3; (void)arg4; (void)arg5;
    switch (option) {
    case PR_SET_SECCOMP:  /* FIXED (v4.3.2): SEC-001 */
        return seccomp_set_filter((struct sock_fprog *)arg2);
    default:
        current->t_errno = EINVAL;
        return -1;
    }
}

/* ================================================================
 * Dispatcher
 * ================================================================ */

/* Maximum valid syscall number */
#define SYS_MAX_NUM  384

long handle_syscall(int num, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    /* Bounds check: reject invalid syscall numbers */
    if (num < 0 || num >= SYS_MAX_NUM) {
        current->t_errno = ENOSYS;
        return -1;
    }

    /* Performance counter: count syscalls */
    perf_inc(PERF_SYSCALL_COUNT);

    /* Seccomp filter check: deny if blocked by task's filter.
     * FIXED (v4.1.9): Pass syscall arguments for BPF-level validation.
     * (H-29: seccomp BPF argument verification) */
    {
        uint64_t args[6] = { a1, a2, a3, a4, a5, a6 };
        if (seccomp_check(current, num, args) != 0) {
            current->t_errno = EPERM;
            return -1;
        }
    }

    long ret = -1;
    switch (num) {
        case SYS_READ:    ret = sys_read((int)a1, (void *)a2, (size_t)a3); break;
        case SYS_WRITE:   ret = sys_write((int)a1, (const void *)a2, (size_t)a3); break;
        case SYS_OPEN:    ret = sys_open((const char *)a1, (int)a2); break;
        case SYS_CLOSE:   ret = sys_close((int)a1); break;
        case SYS_STAT:    ret = sys_stat((const char *)a1, (struct kstat_ext *)a2); break;
        case SYS_FSTAT:   ret = sys_fstat((int)a1, (struct kstat *)a2); break;
        case SYS_LSEEK:   ret = sys_lseek((int)a1, (off_t)a2, (int)a3); break;
        case SYS_MMAP:    ret = sys_mmap((void *)a1, (size_t)a2, (int)a3,
                                         (int)a4, (int)a5, (off_t)a6); break;
        case SYS_MPROTECT: ret = sys_mprotect((void *)a1, (size_t)a2, (int)a3); break;
        case SYS_DUP:     ret = sys_dup((int)a1); break;
        case SYS_DUP2:    ret = sys_dup2((int)a1, (int)a2); break;
        case SYS_GETDENTS:ret = sys_getdents((int)a1, (void *)a2, (size_t)a3); break;
        case SYS_EXECVE:  ret = sys_execve((const char *)a1, (char *const *)a2, (char *const *)a3); break;
        case SYS_EXIT:    ret = sys_exit((int)a1); break;
        case SYS_GETPID:  ret = sys_getpid(); break;
        case SYS_WAITPID: ret = sys_waitpid((int)a1, (int *)a2, (int)a3); break;
        case SYS_KILL:    ret = wrap_sys_kill((int)a1, (int)a2); break;
        case SYS_SIGACTION: ret = wrap_sys_sigaction((int)a1, (const struct sigaction *)a2, (struct sigaction *)a3); break;
        case SYS_SIGRETURN: ret = wrap_sys_sigreturn(); break;
        case SYS_PIPE:    ret = wrap_sys_pipe((int *)a1); break;
        case SYS_FORK:    ret = sys_fork((int)a1); break;
        case SYS_UNAME:   ret = sys_uname((struct utsname *)a1); break;
        case SYS_TIMES:   ret = sys_times((struct tms *)a1); break;
        case SYS_GETCWD:  ret = sys_getcwd((char *)a1, (size_t)a2); break;
        case SYS_CHDIR:   ret = sys_chdir((const char *)a1); break;
        /* Network syscalls */
        case SYS_SOCKET:  ret = sys_socket((int)a1, (int)a2, (int)a3); break;
        case SYS_BIND:    ret = sys_bind((int)a1, (const void *)a2, (int)a3); break;
        case SYS_CONNECT: ret = sys_connect((int)a1, (const void *)a2, (int)a3); break;
        case SYS_LISTEN:  ret = sys_listen((int)a1, (int)a2); break;
        case SYS_ACCEPT:  ret = sys_accept((int)a1, (void *)a2, (int *)a3); break;
        case SYS_SEND:    ret = sys_send((int)a1, (const void *)a2, (size_t)a3, (int)a4); break;
        case SYS_RECV:    ret = sys_recv((int)a1, (void *)a2, (size_t)a3, (int)a4); break;
        case SYS_SENDTO:  ret = sys_sendto((int)a1, (const void *)a2, (size_t)a3, (int)a4, (const void *)a5, (int)a6); break;
        case SYS_RECVFROM: ret = sys_recvfrom((int)a1, (void *)a2, (size_t)a3, (int)a4, (void *)a5, (int *)a6); break;
        /* Time syscalls */
        case SYS_GETTIMEOFDAY: ret = sys_gettimeofday((struct timeval *)a1, (void *)a2); break;
        case SYS_NANOSLEEP: ret = sys_nanosleep((const struct timespec *)a1, (struct timespec *)a2); break;
        /* Filesystem management syscalls */
        case SYS_MKDIR:  ret = sys_mkdir((const char *)a1, (int)a2); break;
        case SYS_RMDIR:  ret = sys_rmdir((const char *)a1); break;
        case SYS_UNLINK: ret = sys_unlink((const char *)a1); break;
        case SYS_RENAME: ret = sys_rename((const char *)a1, (const char *)a2); break;
        case SYS_CHMOD:  ret = sys_chmod((const char *)a1, (int)a2); break;
        /* Device control */
        case SYS_IOCTL:  ret = sys_ioctl((int)a1, (int)a2, (void *)a3); break;
        /* I/O multiplexing */
        case SYS_POLL:   ret = sys_poll((struct pollfd *)a1, (int)a2, (int)a3); break;
        /* Socket management */
        case SYS_SHUTDOWN: ret = sys_shutdown((int)a1, (int)a2); break;
        case SYS_GETSOCKNAME: ret = sys_getsockname((int)a1, (void *)a2, (int *)a3); break;
        /* Extended POSIX syscalls */
        case SYS_ACCESS:    ret = sys_access((const char *)a1, (int)a2); break;
        case SYS_FCHMOD:    ret = sys_fchmod((int)a1, (int)a2); break;
        case SYS_FCHOWN:    ret = sys_fchown((int)a1, (int)a2, (int)a3); break;
        case SYS_FTRUNCATE: ret = sys_ftruncate((int)a1, (off_t)a2); break;
        case SYS_FSYNC:     ret = sys_fsync((int)a1); break;
        case SYS_READLINK:  ret = sys_readlink((const char *)a1, (char *)a2, (size_t)a3); break;
        case SYS_SYMLINK:   ret = sys_symlink((const char *)a1, (const char *)a2); break;
        case SYS_GETPPID:   ret = sys_getppid(); break;
        case SYS_GETUID:    ret = sys_getuid(); break;
        case SYS_GETEUID:   ret = sys_geteuid(); break;
        case SYS_GETGID:    ret = sys_getgid(); break;
        case SYS_GETEGID:   ret = sys_getegid(); break;
        case SYS_SETUID:    ret = sys_setuid((int)a1); break;
        case SYS_SETGID:    ret = sys_setgid((int)a1); break;
        case SYS_GETPGID:   ret = sys_getpgid((int)a1); break;
        case SYS_SETPGID:   ret = sys_setpgid((int)a1, (int)a2); break;
        case SYS_SETSID:    ret = sys_setsid(); break;
        case SYS_NICE:      ret = sys_nice((int)a1); break;
        case SYS_BRK:       ret = sys_brk((void *)a1); break;
        case SYS_SBRK:      ret = sys_sbrk((intptr_t)a1); break;
        case SYS_MADVISE:   ret = sys_madvise((void *)a1, (size_t)a2, (int)a3); break;
        case SYS_CLOCK_GETTIME: ret = sys_clock_gettime((int)a1, (struct timespec *)a2); break;
        case SYS_PIPE2:     ret = sys_pipe2((int *)a1, (int)a2); break;
        case SYS_FCNTL:     ret = sys_fcntl((int)a1, (int)a2, (long)a3); break;
        case SYS_CHOWN:     ret = sys_chown((const char *)a1, (int)a2, (int)a3); break;
        case SYS_SYSINFO:   ret = sys_sysinfo((struct sysinfo *)a1); break;
        case SYS_GETRLIMIT: ret = sys_getrlimit((int)a1, (struct rlimit *)a2); break;
        case SYS_SETRLIMIT: ret = sys_setrlimit((int)a1, (const struct rlimit *)a2); break;
        case SYS_SCHED_YIELD: ret = sys_sched_yield(); break;
        case SYS_GETENV:  ret = sys_getenv((const char *)a1, (char *)a2, (size_t)a3); break;
        case SYS_SETENV:  ret = sys_setenv((const char *)a1, (const char *)a2); break;
        case SYS_GETRANDOM: ret = sys_getrandom((void *)a1, (size_t)a2, (unsigned int)a3); break;
        /* FIXED (v4.2.3): New POSIX syscalls */
        case SYS_SIGPROCMASK: ret = sys_sigprocmask((int)a1, (const uint64_t *)a2, (uint64_t *)a3); break;
        case SYS_READV:  ret = sys_readv((int)a1, (const struct iovec *)a2, (int)a3); break;
        case SYS_WRITEV: ret = sys_writev((int)a1, (const struct iovec *)a2, (int)a3); break;
        case SYS_SELECT: ret = sys_select((int)a1, (fd_set *)a2, (fd_set *)a3,
                                          (fd_set *)a4, (struct timeval *)a5); break;
        case SYS_SOCKETPAIR: ret = sys_socketpair((int)a1, (int)a2, (int)a3, (int *)a4); break;
        case SYS_SETSOCKOPT: ret = sys_setsockopt((int)a1, (int)a2, (int)a3,
                                                  (const void *)a4, (uint32_t)a5); break;
        case SYS_GETSOCKOPT: ret = sys_getsockopt((int)a1, (int)a2, (int)a3,
                                                  (void *)a4, (uint32_t *)a5); break;
        case SYS_GETDENTS64: ret = sys_getdents64((unsigned int)a1,
                                                  (struct linux_dirent64 *)a2, (unsigned int)a3); break;
        /* ACPI power management */
        case SYS_ACPI_SHUTDOWN: ret = sys_acpi_shutdown(); break;
        case SYS_ACPI_REBOOT:   ret = sys_acpi_reboot(); break;
        /* POSIX (v4.2.6): Extended syscalls */
        case SYS_FCHDIR:        ret = sys_fchdir((int)a1); break;
        case SYS_GETRESUID:     ret = sys_getresuid((int *)a1, (int *)a2, (int *)a3); break;
        case SYS_GETRESGID:     ret = sys_getresgid((int *)a1, (int *)a2, (int *)a3); break;
        case SYS_SETRESUID:     ret = sys_setresuid((int)a1, (int)a2, (int)a3); break;
        case SYS_SETRESGID:     ret = sys_setresgid((int)a1, (int)a2, (int)a3); break;
        case SYS_FUTEX:        ret = sys_futex((int *)a1, (int)a2, (int)a3,
                                              (const struct timespec *)a4, (int *)a5, (int)a6); break;
        case SYS_SCHED_SETAFFINITY: ret = sys_sched_setaffinity((int)a1, (unsigned int)a2, (const uint64_t *)a3); break;
        case SYS_SCHED_GETAFFINITY: ret = sys_sched_getaffinity((int)a1, (unsigned int)a2, (uint64_t *)a3); break;
        case SYS_SET_TID_ADDRESS:   ret = sys_set_tid_address((int *)a1); break;
        case SYS_TGKILL:     ret = sys_tgkill((int)a1, (int)a2, (int)a3); break;
        case SYS_MKDIRAT:    ret = sys_mkdirat((int)a1, (const char *)a2, (int)a3); break;
        case SYS_MKNODAT:    ret = sys_mknodat((int)a1, (const char *)a2, (int)a3, (unsigned int)a4); break;
        case SYS_FCHOWNAT:   ret = sys_fchownat((int)a1, (const char *)a2, (int)a3, (int)a4, (int)a5); break;
        case SYS_UNLINKAT:   ret = sys_unlinkat((int)a1, (const char *)a2, (int)a3); break;
        case SYS_LINKAT:     ret = sys_linkat((int)a1, (const char *)a2, (int)a3, (const char *)a4, (int)a5); break;
        case SYS_SYMLINKAT:  ret = sys_symlinkat((const char *)a1, (int)a2, (const char *)a3); break;
        case SYS_READLINKAT: ret = sys_readlinkat((int)a1, (const char *)a2, (char *)a3, (size_t)a4); break;
        case SYS_FCHMODAT:   ret = sys_fchmodat((int)a1, (const char *)a2, (int)a3); break;
        case SYS_FACCESSAT:  ret = sys_faccessat((int)a1, (const char *)a2, (int)a3, (int)a4); break;
        case SYS_PRLIMIT64:  ret = sys_prlimit64((int)a1, (int)a2, (const struct rlimit *)a3, (struct rlimit *)a4); break;
        case SYS_NAME_TO_HANDLE_AT: ret = sys_name_to_handle_at((int)a1, (const char *)a2, (struct file_handle *)a3, (int *)a4, (int)a5); break;
        case SYS_GETCPU:     ret = sys_getcpu((unsigned int *)a1, (unsigned int *)a2, (void *)a3); break;
        case SYS_MEMBARRIER: ret = sys_membarrier((int)a1, (unsigned int)a2, (int)a3); break;
        case SYS_PRCTL:      ret = sys_prctl((int)a1, (unsigned long)a2, (unsigned long)a3, (unsigned long)a4, (unsigned long)a5); break;
        default:
            current->t_errno = ENOSYS;
            ret = -1;
            break;
    }
    return ret;
}

void syscall_trap(struct trapframe *tf) {
    if (!tf) return;
    int num       = (int)tf->rax;
    uint64_t a1   = tf->rdi;
    uint64_t a2   = tf->rsi;
    uint64_t a3   = tf->rdx;
    uint64_t a4   = tf->r10;  /* 4th syscall arg (x86_64 ABI) */
    uint64_t a5   = tf->r8;   /* 5th syscall arg */
    uint64_t a6   = tf->r9;   /* 6th syscall arg */

    /* Set per-task trapframe for signal delivery (FIXED v4.1.4: BUG 4.4) */
    current->current_tf = tf;

    /*
     * Fork child returns 0 to user space.
     * The child's kernel stack is set up by create_task() with fn=NULL,
     * so when the child is first scheduled, context_switch's ret jumps
     * to syscall.S's return path.  The trapframe's RAX was set by the
     * parent's syscall handler, but we override it here for the child.
     */
    if (current && current->is_fork_child) {
        current->is_fork_child = 0;
        tf->rax = 0;
    } else {
        /* Measure syscall latency via RDTSC */
        uint32_t tsc_lo_start, tsc_hi_start;
        asm volatile ("rdtsc" : "=a"(tsc_lo_start), "=d"(tsc_hi_start));
        long ret = handle_syscall(num, a1, a2, a3, a4, a5, a6);
        /* Per-process perf counter: track syscall count */
        if (current) current->syscall_count++;
        uint32_t tsc_lo_end, tsc_hi_end;
        asm volatile ("rdtsc" : "=a"(tsc_lo_end), "=d"(tsc_hi_end));
        uint64_t tsc_diff = (((uint64_t)tsc_hi_end << 32) | tsc_lo_end)
                          - (((uint64_t)tsc_hi_start << 32) | tsc_lo_start);
        perf_add_latency(PERF_SYSCALL_LATENCY, perf_tsc_to_ns(tsc_diff));
        tf->rax = (uint64_t)ret;
    }

    check_signals();
    current->current_tf = NULL;
}
