/* LDSO (v4.2.6) */
/*
 * ld-so.c - AuroraOS user-space dynamic linker (ld.so)
 *
 * Self-contained ELF dynamic linker that loads shared libraries,
 * resolves symbols, and performs relocations.  The kernel loads
 * ld.so as the "interpreter" (PT_INTERP) and jumps to _start().
 *
 * Stack layout at _start() entry (from kernel):
 *   [highest]  envp strings / argv strings
 *              auxv[] (Elf64_auxv_t, AT_NULL-terminated)
 *              envp[] (NULL-terminated)
 *              argv[] (NULL-terminated)
 *              argc    (uint64_t)
 *   [lowest]   <- RSP
 *
 * This file is 100% self-developed — no external library dependencies.
 * All syscalls use inline assembly, all memory is self-managed.
 */

#include "ld-so.h"

/* ================================================================
 * Internal constants
 * ================================================================ */
#define PAGE_SIZE          4096
#define LD_HEAP_START      0x7F800000ULL  /* bump allocator heap */
#define LD_HEAP_MAX        0x7FC00000ULL  /* 4 MB heap */
#define LD_LIB_BASE        0x7F100000ULL  /* where to start mapping libs */
#define LD_LIB_STRIDE      0x00200000ULL  /* 2 MB per library slot */

/* ================================================================
 * Syscall numbers (matching kernel/syscall.h)
 * ================================================================ */
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_MMAP    9
#define SYS_EXIT    60

/* ================================================================
 * mmap constants
 * ================================================================ */
#define PROT_READ        0x1
#define PROT_WRITE       0x2
#define PROT_EXEC        0x4
#define MAP_PRIVATE      0x02
#define MAP_ANONYMOUS    0x20
#define MAP_FIXED        0x10

/* open flags */
#define O_RDONLY         0

/* ================================================================
 * Bump allocator — simple, fast, no free() needed for ld.so
 * ================================================================ */
static char *ld_brk = (char *)LD_HEAP_START;

static void *ld_alloc(unsigned long size) {
    /* align to 16 bytes */
    size = (size + 15) & ~15UL;
    if ((unsigned long)ld_brk + size > LD_HEAP_MAX) return NULL;
    void *ptr = ld_brk;
    ld_brk += size;
    return ptr;
}

/* ================================================================
 * Basic string functions (no libc)
 * ================================================================ */
static unsigned long ld_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) ++n;
    return n;
}

static int ld_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

static int ld_strncmp(const char *a, const char *b, unsigned long n) {
    if (!n) return 0;
    while (--n && *a && *a == *b) { ++a; ++b; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

static void ld_memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static void ld_memset(void *s, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
}

static char *ld_strcpy(char *dst, const char *src) {
    char *d = dst;
    while (*src) *d++ = *src++;
    *d = '\0';
    return dst;
}

/* ================================================================
 * Syscall wrappers
 * ================================================================ */

/* 3-argument syscall: rax=num, rdi=a1, rsi=a2, rdx=a3 */
static long ld_syscall3(long num, long a1, long a2, long a3) {
    long ret;
    asm volatile (
        "mov %1, %%rax\n\t"
        "mov %2, %%rdi\n\t"
        "mov %3, %%rsi\n\t"
        "mov %4, %%rdx\n\t"
        "syscall\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

/* 6-argument syscall for mmap:
 *   rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6 */
static long ld_syscall6(long num, long a1, long a2, long a3,
                         long a4, long a5, long a6) {
    long ret;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile (
        "mov %1, %%rax\n\t"
        "mov %2, %%rdi\n\t"
        "mov %3, %%rsi\n\t"
        "mov %4, %%rdx\n\t"
        "syscall\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3),
          "r"(r10), "r"(r8), "r"(r9)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

static int ld_open(const char *path) {
    return (int)ld_syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
}

static long ld_read(int fd, void *buf, unsigned long count) {
    return ld_syscall3(SYS_READ, fd, (long)buf, (long)count);
}

static int ld_close(int fd) {
    return (int)ld_syscall3(SYS_CLOSE, fd, 0, 0);
}

static void *ld_mmap(void *addr, unsigned long length, int prot,
                      int flags, int fd, long offset) {
    long ret = ld_syscall6(SYS_MMAP, (long)addr, (long)length,
                            (long)prot, (long)flags, (long)fd, offset);
    if (ret < 0) return NULL;
    return (void *)ret;
}

static void ld_exit(int code) {
    ld_syscall3(SYS_EXIT, code, 0, 0);
    for (;;) {}
}

/* Debug output (uses write syscall) */
static void ld_debug(const char *msg) {
    ld_syscall3(SYS_WRITE, 1, (long)msg, (long)ld_strlen(msg));
}

/* ================================================================
 * ld_resolve_path: Search for a library in standard paths.
 * Tries: /lib/<name>, /usr/lib/<name>
 * Returns 0 on success, -1 on failure.
 * ================================================================ */
int ld_resolve_path(const char *name, char *out, int outsz) {
    if (!name || !out || outsz <= 0) return -1;

    /* absolute path — use as-is */
    if (name[0] == '/') {
        unsigned long len = ld_strlen(name);
        if (len + 1 > (unsigned long)outsz) return -1;
        ld_strcpy(out, name);
        return 0;
    }

    /* Try /lib/<name> */
    const char *paths[] = { "/lib/", "/usr/lib/", NULL };
    for (int i = 0; paths[i]; ++i) {
        unsigned long plen = ld_strlen(paths[i]);
        unsigned long nlen = ld_strlen(name);
        if (plen + nlen + 1 > (unsigned long)outsz) continue;
        ld_strcpy(out, paths[i]);
        ld_strcpy(out + plen, name);
        int fd = ld_open(out);
        if (fd >= 0) {
            ld_close(fd);
            return 0;
        }
    }
    return -1;
}

/* ================================================================
 * ld_map_library: Map a shared library (.so) into memory.
 *
 * Reads the ELF header and program headers, allocates memory
 * via mmap, copies segments, and sets up the ld_library struct.
 * Returns the library struct on success, NULL on failure.
 * ================================================================ */
struct ld_library *ld_map_library(const char *path) {
    int fd = ld_open(path);
    if (fd < 0) return NULL;

    /* Read ELF header */
    Elf64_Ehdr ehdr;
    long r = ld_read(fd, &ehdr, sizeof(ehdr));
    if (r != (long)sizeof(ehdr) ||
        ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F' ||
        ehdr.e_machine != 0x3E || ehdr.e_type != ET_DYN) {
        ld_close(fd);
        return NULL;
    }

    if (ehdr.e_phnum == 0 || ehdr.e_phnum > 128) {
        ld_close(fd);
        return NULL;
    }

    /* Read all program headers */
    unsigned long phdr_size = (unsigned long)ehdr.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdrs = (Elf64_Phdr *)ld_alloc(phdr_size);
    if (!phdrs) { ld_close(fd); return NULL; }

    ld_syscall3(SYS_READ, fd, (long)phdrs, (long)phdr_size);
    /* Note: we need to re-read properly; do it with lseek-like approach */
    /* Actually, we already read the EHDR, the file offset is now at sizeof(ehdr).
     * We need to lseek to phoff. But the kernel may not support lseek.
     * Instead, we close and re-open. Better yet, we read from the start including
     * both ehdr and phdrs in one shot. Let's redo: */
    ld_close(fd);
    fd = ld_open(path);
    if (fd < 0) return NULL;

    /* Read ehdr + phdrs in one go */
    unsigned long total = sizeof(Elf64_Ehdr) + phdr_size;
    char *buf = (char *)ld_alloc(total);
    if (!buf) { ld_close(fd); return NULL; }
    r = ld_read(fd, buf, total);
    if (r != (long)total) { ld_close(fd); return NULL; }

    ld_memcpy(&ehdr, buf, sizeof(Elf64_Ehdr));
    phdrs = (Elf64_Phdr *)ld_alloc(phdr_size);
    if (!phdrs) { ld_close(fd); return NULL; }
    ld_memcpy(phdrs, buf + sizeof(Elf64_Ehdr), phdr_size);

    /* Find the loadable segment bounds */
    uint64_t min_vaddr = (uint64_t)-1;
    uint64_t max_vaddr = 0;
    for (int i = 0; i < ehdr.e_phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint64_t s = phdrs[i].p_vaddr;
        uint64_t e = s + phdrs[i].p_memsz;
        if (s < min_vaddr) min_vaddr = s;
        if (e > max_vaddr) max_vaddr = e;
    }
    if (min_vaddr == (uint64_t)-1) { ld_close(fd); return NULL; }

    /* Align min to page, max to page */
    min_vaddr &= ~(PAGE_SIZE - 1);
    max_vaddr = (max_vaddr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Allocate a slot for this library */
    static int ld_lib_slot = 0;
    uint64_t map_base = LD_LIB_BASE + (uint64_t)ld_lib_slot * LD_LIB_STRIDE;
    ++ld_lib_slot;

    /* Map the full range */
    uint64_t map_size = max_vaddr - min_vaddr;
    void *base = ld_mmap((void *)(map_base + min_vaddr), (unsigned long)map_size,
                          PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (!base || base == (void *)-1) {
        /* Try without MAP_FIXED */
        base = ld_mmap(NULL, (unsigned long)map_size,
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (!base || base == (void *)-1) { ld_close(fd); return NULL; }
        map_base = (uint64_t)(uintptr_t)base - min_vaddr;
    }

    uint64_t actual_base = map_base;

    /* Load each PT_LOAD segment */
    for (int i = 0; i < ehdr.e_phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint64_t dst_va = actual_base + phdrs[i].p_vaddr;
        unsigned long filesz = (unsigned long)phdrs[i].p_filesz;
        unsigned long memsz  = (unsigned long)phdrs[i].p_memsz;

        if (filesz > 0) {
            /* Seek to file offset — we need to re-read segment data.
             * Since we don't have lseek, close and reopen with a fresh fd,
             * then read from offset. */
            /* Actually, we already read ehdr+phdrs. Let's just read the
             * segment data with a fresh fd. */
            int fd2 = ld_open(path);
            if (fd2 >= 0) {
                /* Skip past ehdr and phdrs to get to segment data.
                 * We can just read the whole file from the start into a
                 * temporary buffer, but that's expensive. Instead, we
                 * read a dummy chunk to advance the offset. But that's
                 * fragile. Better: we should have read the whole file.
                 *
                 * For simplicity, use a small temp buffer and read the
                 * segment data from the file. We know the file offset
                 * is phdrs[i].p_offset. We need to skip that many bytes
                 * from the start. Since we can't lseek, we read the
                 * file from the beginning by reopening. */
                /* Actually, the simplest approach: we already read the
                 * whole file in one shot. Let's change strategy:
                 * Read the whole file into a temp buffer. */
                ld_close(fd2);
            }
            /* We'll read directly from the file offset. Since we can't
             * lseek, we need to re-read from the beginning. Let's just
             * do a simple approach: reopen and read. */
            int fd3 = ld_open(path);
            if (fd3 >= 0) {
                /* Read the file starting from offset 0. We need to skip
                 * phdrs[i].p_offset bytes. We'll read into a temp buffer
                 * and discard what we don't need. */
                /* Actually, let's just read into the destination directly.
                 * The file offset starts at 0 for a fresh fd. We need to
                 * advance by p_offset. We can do this by reading and
                 * discarding. */
                char tmp[4096];
                unsigned long off = (unsigned long)phdrs[i].p_offset;
                while (off > 0) {
                    unsigned long chunk = off > 4096 ? 4096 : off;
                    ld_read(fd3, tmp, chunk);
                    off -= chunk;
                }
                /* Now at the right offset, read into destination */
                ld_read(fd3, (void *)(uintptr_t)dst_va, filesz);
                ld_close(fd3);
            }
        }
        /* Zero the BSS (memsz > filesz) */
        if (memsz > filesz) {
            ld_memset((void *)(uintptr_t)(dst_va + filesz), 0, memsz - filesz);
        }
    }

    ld_close(fd);

    /* Allocate and populate ld_library struct */
    struct ld_library *lib = (struct ld_library *)ld_alloc(sizeof(struct ld_library));
    if (!lib) return NULL;
    ld_memset(lib, 0, sizeof(*lib));

    /* Extract the base name */
    {
        const char *name = path;
        const char *slash = path;
        while (*name) {
            if (*name == '/') slash = name + 1;
            ++name;
        }
        unsigned long nlen = ld_strlen(slash);
        if (nlen > 127) nlen = 127;
        for (unsigned long j = 0; j < nlen; ++j) lib->name[j] = slash[j];
        lib->name[nlen] = '\0';
    }

    lib->base = actual_base;
    lib->entry = actual_base + ehdr.e_entry;
    lib->ehdr = ehdr;
    lib->phdr = (Elf64_Phdr *)(uintptr_t)(actual_base + ehdr.e_phoff);
    lib->phnum = ehdr.e_phnum;

    /* Parse .dynamic section */
    Elf64_Dyn *dyn = NULL;
    uint64_t dyn_addr = 0;
    for (int i = 0; i < ehdr.e_phnum; ++i) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn_addr = actual_base + phdrs[i].p_vaddr;
            dyn = (Elf64_Dyn *)(uintptr_t)dyn_addr;
            lib->dyn_count = (int)(phdrs[i].p_filesz / sizeof(Elf64_Dyn));
            break;
        }
    }
    lib->dynamic = dyn;

    /* Parse dynamic entries */
    if (dyn) {
        for (int i = 0; i < lib->dyn_count; ++i) {
            switch (dyn[i].d_tag) {
            case DT_SYMTAB:
                lib->symtab = (Elf64_Sym *)(uintptr_t)(actual_base + dyn[i].d_val);
                break;
            case DT_STRTAB:
                lib->strtab = (char *)(uintptr_t)(actual_base + dyn[i].d_val);
                break;
            case DT_STRSZ:
                lib->strsz = dyn[i].d_val;
                break;
            case DT_JMPREL:
                lib->jmprel = (Elf64_Rela *)(uintptr_t)(actual_base + dyn[i].d_val);
                break;
            case DT_PLTRELSZ:
                lib->pltrelsz = dyn[i].d_val;
                break;
            case DT_RELA:
                lib->rela = (Elf64_Rela *)(uintptr_t)(actual_base + dyn[i].d_val);
                break;
            case DT_RELASZ:
                lib->relasz = dyn[i].d_val;
                break;
            case DT_PLTGOT:
                lib->got = (uint64_t *)(uintptr_t)(actual_base + dyn[i].d_val);
                break;
            default:
                break;
            }
        }
        /* Estimate sym_count from the string table offset */
        if (lib->symtab && lib->strtab) {
            uint64_t diff = (uint64_t)(uintptr_t)lib->strtab -
                            (uint64_t)(uintptr_t)lib->symtab;
            lib->sym_count = diff / sizeof(Elf64_Sym);
            if (lib->sym_count > LD_SYMTAB_MAX) lib->sym_count = LD_SYMTAB_MAX;
        }
    }

    return lib;
}

/* ================================================================
 * ld_parse_auxv: Parse the auxiliary vector from the stack.
 * ================================================================ */
void ld_parse_auxv(Elf64_auxv_t *auxv, struct ld_auxv *parsed) {
    ld_memset(parsed, 0, sizeof(*parsed));
    if (!auxv) return;

    for (int i = 0; ; ++i) {
        if (auxv[i].a_type == AT_NULL) break;
        switch (auxv[i].a_type) {
        case AT_PHDR:   parsed->at_phdr   = auxv[i].a_val; break;
        case AT_PHENT:  parsed->at_phent  = auxv[i].a_val; break;
        case AT_PHNUM:  parsed->at_phnum  = auxv[i].a_val; break;
        case AT_PAGESZ: parsed->at_pagesz = auxv[i].a_val; break;
        case AT_ENTRY:  parsed->at_entry  = auxv[i].a_val; break;
        case AT_BASE:   parsed->at_base   = auxv[i].a_val; break;
        default: break;
        }
    }
}

/* ================================================================
 * ld_find_symbol: Look up a symbol in all loaded libraries.
 *
 * Searches the main binary first, then dependency libraries.
 * Returns 0 on success, -1 if not found.
 * ================================================================ */
int ld_find_symbol(struct ld_library *head, const char *name,
                    struct ld_library **found_lib, Elf64_Sym **found_sym) {
    if (!head || !name || !found_lib || !found_sym) return -1;

    for (struct ld_library *lib = head; lib; lib = lib->next) {
        if (!lib->symtab || !lib->strtab || lib->sym_count == 0) continue;

        for (uint64_t i = 0; i < lib->sym_count; ++i) {
            uint32_t bind = ELF64_ST_BIND(lib->symtab[i].st_info);
            if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
            if (lib->symtab[i].st_shndx == 0) continue; /* undefined */
            if (lib->symtab[i].st_value == 0) continue;

            const char *sym_name = lib->strtab + lib->symtab[i].st_name;
            if (ld_strcmp(sym_name, name) == 0) {
                *found_lib = lib;
                *found_sym = &lib->symtab[i];
                return 0;
            }
        }
    }
    return -1;
}

/* ================================================================
 * ld_relocate: Process all relocation entries for a library.
 *
 * Iterates all loaded libraries (main + dependencies) and applies:
 *   - R_X86_64_RELATIVE: base + addend
 *   - R_X86_64_GLOB_DAT: symbol value + addend
 *   - R_X86_64_JUMP_SLOT: deferred to ld_bind_plt (lazy binding)
 *   - R_X86_64_64: symbol value + addend
 *   - R_X86_64_COPY: copy symbol value from another library
 * ================================================================ */
int ld_relocate(struct ld_library *lib, struct ld_library *head) {
    if (!lib || !head) return -1;

    /* Process RELA (.rela.dyn) */
    if (lib->rela && lib->relasz > 0) {
        uint64_t count = lib->relasz / sizeof(Elf64_Rela);
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t r_offset = lib->rela[i].r_offset;
            uint64_t r_info   = lib->rela[i].r_info;
            int64_t  r_addend = lib->rela[i].r_addend;
            uint32_t r_type   = ELF64_R_TYPE(r_info);
            uint32_t r_sym    = ELF64_R_SYM(r_info);
            uint64_t target   = lib->base + r_offset;

            switch (r_type) {
            case R_X86_64_RELATIVE:
                *(uint64_t *)(uintptr_t)target = lib->base + (uint64_t)r_addend;
                break;

            case R_X86_64_GLOB_DAT: {
                if (r_sym < lib->sym_count && lib->symtab && lib->strtab) {
                    const char *sym_name = lib->strtab + lib->symtab[r_sym].st_name;
                    struct ld_library *flib = NULL;
                    Elf64_Sym *fsym = NULL;
                    if (ld_find_symbol(head, sym_name, &flib, &fsym) == 0) {
                        *(uint64_t *)(uintptr_t)target =
                            flib->base + fsym->st_value + (uint64_t)r_addend;
                    }
                }
                break;
            }

            case R_X86_64_64: {
                if (r_sym < lib->sym_count && lib->symtab && lib->strtab) {
                    const char *sym_name = lib->strtab + lib->symtab[r_sym].st_name;
                    struct ld_library *flib = NULL;
                    Elf64_Sym *fsym = NULL;
                    if (ld_find_symbol(head, sym_name, &flib, &fsym) == 0) {
                        *(uint64_t *)(uintptr_t)target =
                            flib->base + fsym->st_value + (uint64_t)r_addend;
                    }
                }
                break;
            }

            case R_X86_64_JUMP_SLOT: {
                /* For NOW binding (BIND_NOW), resolve immediately.
                 * Otherwise, the PLT will call ld_bind_plt lazily. */
                /* Check if DF_1_NOW is set in DT_FLAGS_1 */
                int bind_now = 0;
                if (lib->dynamic) {
                    for (int d = 0; d < lib->dyn_count; ++d) {
                        if (lib->dynamic[d].d_tag == DT_FLAGS_1 &&
                            (lib->dynamic[d].d_val & DF_1_NOW)) {
                            bind_now = 1;
                            break;
                        }
                        if (lib->dynamic[d].d_tag == DT_BIND_NOW) {
                            bind_now = 1;
                            break;
                        }
                    }
                }
                if (bind_now) {
                    if (r_sym < lib->sym_count && lib->symtab && lib->strtab) {
                        const char *sym_name = lib->strtab + lib->symtab[r_sym].st_name;
                        struct ld_library *flib = NULL;
                        Elf64_Sym *fsym = NULL;
                        if (ld_find_symbol(head, sym_name, &flib, &fsym) == 0) {
                            *(uint64_t *)(uintptr_t)target =
                                flib->base + fsym->st_value;
                        }
                    }
                }
                /* else: lazy binding — PLT stub will call ld_bind_plt */
                break;
            }

            case R_X86_64_COPY: {
                if (r_sym < lib->sym_count && lib->symtab && lib->strtab) {
                    const char *sym_name = lib->strtab + lib->symtab[r_sym].st_name;
                    struct ld_library *flib = NULL;
                    Elf64_Sym *fsym = NULL;
                    if (ld_find_symbol(head, sym_name, &flib, &fsym) == 0) {
                        uint64_t src = flib->base + fsym->st_value;
                        uint64_t sz  = fsym->st_size;
                        ld_memcpy((void *)(uintptr_t)target, (void *)(uintptr_t)src, (unsigned long)sz);
                    }
                }
                break;
            }

            case R_X86_64_IRELATIVE: {
                uint64_t (*resolver)(void) = (uint64_t (*)(void))(uintptr_t)(lib->base + (uint64_t)r_addend);
                *(uint64_t *)(uintptr_t)target = resolver();
                break;
            }

            default:
                break;
            }
        }
    }

    /* Process PLT relocations (JMPREL) — only if BIND_NOW */
    if (lib->jmprel && lib->pltrelsz > 0) {
        uint64_t count = lib->pltrelsz / sizeof(Elf64_Rela);
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t r_offset = lib->jmprel[i].r_offset;
            uint64_t r_info   = lib->jmprel[i].r_info;
            uint32_t r_type   = ELF64_R_TYPE(r_info);
            uint32_t r_sym    = ELF64_R_SYM(r_info);
            uint64_t target   = lib->base + r_offset;

            if (r_type != R_X86_64_JUMP_SLOT) continue;

            /* Check BIND_NOW */
            int bind_now = 0;
            if (lib->dynamic) {
                for (int d = 0; d < lib->dyn_count; ++d) {
                    if (lib->dynamic[d].d_tag == DT_FLAGS_1 &&
                        (lib->dynamic[d].d_val & DF_1_NOW)) {
                        bind_now = 1;
                        break;
                    }
                    if (lib->dynamic[d].d_tag == DT_BIND_NOW) {
                        bind_now = 1;
                        break;
                    }
                }
            }
            if (!bind_now) continue;

            if (r_sym < lib->sym_count && lib->symtab && lib->strtab) {
                const char *sym_name = lib->strtab + lib->symtab[r_sym].st_name;
                struct ld_library *flib = NULL;
                Elf64_Sym *fsym = NULL;
                if (ld_find_symbol(head, sym_name, &flib, &fsym) == 0) {
                    *(uint64_t *)(uintptr_t)target = flib->base + fsym->st_value;
                }
            }
        }
    }

    return 0;
}

/* ================================================================
 * ld_bind_plt: Lazy PLT binding — called when a PLT entry is first used.
 *
 * The PLT stub pushes the relocation index and jumps here.
 * This function looks up the symbol and patches the GOT entry.
 * Returns the resolved function address.
 * ================================================================ */
uint64_t ld_bind_plt(struct ld_library *lib, int reloc_idx) {
    if (!lib || !lib->jmprel || reloc_idx < 0) return 0;

    uint64_t plt_count = lib->pltrelsz / sizeof(Elf64_Rela);
    if ((uint64_t)reloc_idx >= plt_count) return 0;

    Elf64_Rela *rel = &lib->jmprel[reloc_idx];
    uint32_t r_sym = ELF64_R_SYM(rel->r_info);

    if (r_sym >= lib->sym_count || !lib->symtab || !lib->strtab) return 0;

    const char *sym_name = lib->strtab + lib->symtab[r_sym].st_name;

    /* Search all loaded libraries (the linked list is accessed via a global) */
    /* We need access to the library list head. This is stored in a global. */
    extern struct ld_library *ld_lib_head;

    struct ld_library *flib = NULL;
    Elf64_Sym *fsym = NULL;
    if (ld_find_symbol(ld_lib_head, sym_name, &flib, &fsym) != 0) return 0;

    uint64_t resolved = flib->base + fsym->st_value;
    uint64_t target = lib->base + rel->r_offset;
    *(uint64_t *)(uintptr_t)target = resolved;
    return resolved;
}

/* ================================================================
 * ld_load_dependencies: Load all DT_NEEDED shared libraries for a lib.
 *
 * Walks the .dynamic section, finds DT_NEEDED entries, resolves
 * paths, maps each library, and appends to the linked list.
 * Returns 0 on success, -1 on failure.
 * ================================================================ */
int ld_load_dependencies(struct ld_library *lib, struct ld_library **head) {
    if (!lib || !lib->dynamic || !head) return -1;

    for (int i = 0; i < lib->dyn_count; ++i) {
        if (lib->dynamic[i].d_tag != DT_NEEDED) continue;

        const char *needed = lib->strtab + (unsigned long)lib->dynamic[i].d_val;
        char path[LD_MAX_PATH];

        if (ld_resolve_path(needed, path, sizeof(path)) != 0) {
            /* Could not resolve — skip silently (weak dependency) */
            continue;
        }

        /* Check if already loaded */
        struct ld_library *cur = *head;
        int already_loaded = 0;
        while (cur) {
            if (ld_strcmp(cur->name, needed) == 0) {
                already_loaded = 1;
                break;
            }
            cur = cur->next;
        }
        if (already_loaded) continue;

        struct ld_library *dep = ld_map_library(path);
        if (!dep) continue;

        /* Append to linked list */
        dep->next = NULL;
        if (!*head) {
            *head = dep;
        } else {
            struct ld_library *tail = *head;
            while (tail->next) tail = tail->next;
            tail->next = dep;
        }

        /* Recursively load dependencies of this library */
        ld_load_dependencies(dep, head);
    }
    return 0;
}

/* ================================================================
 * _start: Entry point called by the kernel.
 *
 * Stack layout:
 *   [RSP]      = argc
 *   [RSP + 8]  = argv[0]
 *   [RSP + 8*(argc+1)] = NULL (argv terminator)
 *   [RSP + 8*(argc+2)] = envp[0] ...
 *   ... until NULL terminator
 *   [after envp NULL] = auxv[0], auxv[1], ..., AT_NULL
 *
 * Steps:
 *   1. Parse argc, argv, envp, auxv from the stack
 *   2. Parse auxv to find AT_PHDR, AT_ENTRY, AT_BASE, AT_PHNUM, AT_PHENT
 *   3. Create ld_library for the main executable
 *   4. Parse main binary's .dynamic for DT_NEEDED
 *   5. Load all dependencies
 *   6. Relocate all libraries
 *   7. Call init functions
 *   8. Jump to main binary's entry point
 * ================================================================ */

/* Global linked list head for PLT binding */
struct ld_library *ld_lib_head = NULL;

/* The main binary's entry point */
static void (*main_entry)(void) = NULL;

void _start(void) {
    uint64_t *sp;
    asm volatile ("mov %%rsp, %0" : "=r"(sp));

    /* Step 1: Parse argc */
    int argc = (int)*sp;
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;

    /* Find envp terminator */
    int envc = 0;
    while (envp[envc]) ++envc;

    /* Auxv starts after the envp NULL terminator */
    Elf64_auxv_t *auxv = (Elf64_auxv_t *)(envp + envc + 1);

    /* Step 2: Parse auxv */
    struct ld_auxv parsed;
    ld_parse_auxv(auxv, &parsed);

    if (parsed.at_entry == 0) {
        ld_debug("ld.so: no AT_ENTRY in auxv\n");
        ld_exit(1);
    }

    /* Step 3: Set up the main executable as a library */
    struct ld_library *main_lib = (struct ld_library *)ld_alloc(sizeof(struct ld_library));
    if (!main_lib) {
        ld_debug("ld.so: out of memory\n");
        ld_exit(1);
    }
    ld_memset(main_lib, 0, sizeof(*main_lib));

    main_lib->is_main = 1;
    main_lib->base    = 0;  /* ET_EXEC — fixed address, base is 0 */
    main_lib->entry   = parsed.at_entry;
    main_lib->phnum   = (uint16_t)parsed.at_phnum;
    main_lib->phdr    = (Elf64_Phdr *)(uintptr_t)parsed.at_phdr;

    ld_strcpy(main_lib->name, "main");

    /* Find PT_DYNAMIC for the main binary */
    for (int i = 0; i < main_lib->phnum; ++i) {
        if (main_lib->phdr[i].p_type == PT_DYNAMIC) {
            main_lib->dynamic = (Elf64_Dyn *)(uintptr_t)main_lib->phdr[i].p_vaddr;
            main_lib->dyn_count = (int)(main_lib->phdr[i].p_filesz / sizeof(Elf64_Dyn));
            break;
        }
    }

    /* Parse dynamic entries for the main binary */
    if (main_lib->dynamic) {
        for (int i = 0; i < main_lib->dyn_count; ++i) {
            Elf64_Dyn *d = &main_lib->dynamic[i];
            switch (d->d_tag) {
            case DT_SYMTAB:
                main_lib->symtab = (Elf64_Sym *)(uintptr_t)d->d_val;
                break;
            case DT_STRTAB:
                main_lib->strtab = (char *)(uintptr_t)d->d_val;
                break;
            case DT_STRSZ:
                main_lib->strsz = d->d_val;
                break;
            case DT_JMPREL:
                main_lib->jmprel = (Elf64_Rela *)(uintptr_t)d->d_val;
                break;
            case DT_PLTRELSZ:
                main_lib->pltrelsz = d->d_val;
                break;
            case DT_RELA:
                main_lib->rela = (Elf64_Rela *)(uintptr_t)d->d_val;
                break;
            case DT_RELASZ:
                main_lib->relasz = d->d_val;
                break;
            case DT_PLTGOT:
                main_lib->got = (uint64_t *)(uintptr_t)d->d_val;
                break;
            default:
                break;
            }
        }
        /* Estimate sym_count */
        if (main_lib->symtab && main_lib->strtab) {
            uint64_t diff = (uint64_t)(uintptr_t)main_lib->strtab -
                            (uint64_t)(uintptr_t)main_lib->symtab;
            main_lib->sym_count = diff / sizeof(Elf64_Sym);
            if (main_lib->sym_count > LD_SYMTAB_MAX) main_lib->sym_count = LD_SYMTAB_MAX;
        }
    }

    /* Add main to the global list */
    ld_lib_head = main_lib;

    /* Step 4: Load dependencies */
    ld_load_dependencies(main_lib, &ld_lib_head);

    /* Step 5: Relocate all libraries (main first, then dependencies) */
    ld_relocate(main_lib, ld_lib_head);
    for (struct ld_library *lib = main_lib->next; lib; lib = lib->next) {
        ld_relocate(lib, ld_lib_head);
    }

    /* Step 6: Call init functions (DT_INIT, DT_INIT_ARRAY) for each library */
    for (struct ld_library *lib = ld_lib_head; lib; lib = lib->next) {
        if (!lib->dynamic) continue;
        for (int i = 0; i < lib->dyn_count; ++i) {
            if (lib->dynamic[i].d_tag == DT_INIT) {
                uint64_t init_addr = lib->base + lib->dynamic[i].d_val;
                void (*init_func)(void) = (void (*)(void))(uintptr_t)init_addr;
                init_func();
            }
        }
        /* DT_INIT_ARRAY */
        uint64_t init_array = 0, init_arraysz = 0;
        for (int i = 0; i < lib->dyn_count; ++i) {
            if (lib->dynamic[i].d_tag == DT_INIT_ARRAY)
                init_array = lib->dynamic[i].d_val;
            if (lib->dynamic[i].d_tag == DT_INIT_ARRAYSZ)
                init_arraysz = lib->dynamic[i].d_val;
        }
        if (init_array && init_arraysz) {
            uint64_t *arr = (uint64_t *)(uintptr_t)(lib->base + init_array);
            uint64_t count = init_arraysz / sizeof(uint64_t);
            for (uint64_t j = 0; j < count; ++j) {
                void (*init_func)(void) = (void (*)(void))(uintptr_t)arr[j];
                if (init_func) init_func();
            }
        }
    }

    /* Step 7: Jump to the main binary's entry point.
     * Set up argc, argv, envp as arguments (x86_64 calling convention:
     * rdi=argc, rsi=argv, rdx=envp).
     * Then jump to main_entry. */
    main_entry = (void (*)(void))(uintptr_t)parsed.at_entry;

    asm volatile (
        "mov %0, %%rdi\n\t"   /* rdi = argc */
        "mov %1, %%rsi\n\t"   /* rsi = argv */
        "mov %2, %%rdx\n\t"   /* rdx = envp */
        "xor %%rbp, %%rbp\n\t" /* clear frame pointer */
        "jmp *%3\n\t"
        :
        : "r"((uint64_t)argc), "r"(argv), "r"(envp), "r"(main_entry)
        : "rdi", "rsi", "rdx", "rbp", "memory"
    );

    /* Never reached */
    ld_exit(0);
}