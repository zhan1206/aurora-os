/* LDSO (v4.2.6) */
/*
 * ld-so.h - AuroraOS user-space dynamic linker header
 *
 * Defines data structures for the dynamic linker: ELF relocation
 * constants, loaded library tracking, and auxiliary vector entries.
 * 100% self-developed — no external library dependencies.
 */
#ifndef LD_SO_H
#define LD_SO_H

#include <stdint.h>

/* ================================================================
 * x86_64 ELF relocation types
 * ================================================================ */
#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_PC32       2
#define R_X86_64_COPY       5
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8
#define R_X86_64_IRELATIVE  37

/* ================================================================
 * ELF type / segment / section constants
 * ================================================================ */
#define ET_DYN        3

#define PT_NULL       0
#define PT_LOAD       1
#define PT_DYNAMIC    2
#define PT_INTERP     3
#define PT_PHDR       6
#define PT_TLS        7
#define PT_GNU_RELRO  0x6474e552

#define PF_X  1
#define PF_W  2
#define PF_R  4

#define DT_NULL       0
#define DT_NEEDED     1
#define DT_PLTRELSZ   2
#define DT_PLTGOT     3
#define DT_HASH       4
#define DT_STRTAB     5
#define DT_SYMTAB     6
#define DT_RELA       7
#define DT_RELASZ     8
#define DT_RELAENT    9
#define DT_STRSZ      10
#define DT_SYMENT     11
#define DT_INIT       12
#define DT_FINI       13
#define DT_JMPREL     23
#define DT_BIND_NOW   24
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_FLAGS      30
#define DT_GNU_HASH   0x6ffffef5
#define DT_FLAGS_1    0x6ffffffb

#define DF_1_NOW      0x00000001

/* ================================================================
 * Auxiliary vector constants
 * ================================================================ */
#define AT_NULL       0
#define AT_PHDR       3
#define AT_PHENT      4
#define AT_PHNUM      5
#define AT_PAGESZ     6
#define AT_BASE       7
#define AT_ENTRY      9

/* ================================================================
 * ELF structures (freestanding, no system headers)
 * ================================================================ */
#define EI_NIDENT     16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} Elf64_Dyn;

typedef struct {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

/* Symbol binding */
#define ELF64_R_SYM(i)      ((i) >> 32)
#define ELF64_R_TYPE(i)     ((i) & 0xFFFFFFFF)
#define ELF64_ST_BIND(i)    ((i) >> 4)
#define ELF64_ST_TYPE(i)    ((i) & 0xF)
#define ELF64_ST_VISIBILITY(i) ((i) & 0x3)

#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STB_WEAK    2
#define STT_FUNC    2
#define STT_OBJECT  1
#define STV_DEFAULT 0
#define STV_HIDDEN  2

/* ================================================================
 * Auxiliary vector entry
 * ================================================================ */
typedef struct {
    uint64_t a_type;
    uint64_t a_val;
} Elf64_auxv_t;

/* ================================================================
 * Loaded library tracking
 * ================================================================ */
#define LD_MAX_LIBS         32
#define LD_MAX_NEEDED       64
#define LD_MAX_PATH         256
#define LD_SYMTAB_MAX       4096
#define LD_STRSZ_MAX        65536

struct ld_library {
    char     name[128];               /* library name (e.g., "libc.so") */
    uint64_t base;                    /* load base virtual address */
    uint64_t entry;                   /* entry point (for the main binary) */
    Elf64_Ehdr ehdr;                  /* ELF header (cached) */
    Elf64_Phdr *phdr;                 /* pointer to program headers in memory */
    uint16_t phnum;                   /* number of program headers */
    Elf64_Dyn *dynamic;               /* pointer to .dynamic section */
    int      dyn_count;               /* number of dynamic entries */
    Elf64_Sym *symtab;                /* pointer to symbol table (.dynsym) */
    uint64_t sym_count;               /* number of symbol table entries */
    char     *strtab;                 /* pointer to string table (.dynstr) */
    uint64_t strsz;                   /* size of string table */
    Elf64_Rela *jmprel;               /* PLT relocations (.rela.plt) */
    uint64_t pltrelsz;                /* size of PLT relocations */
    Elf64_Rela *rela;                 /* regular relocations (.rela.dyn) */
    uint64_t relasz;                  /* size of regular relocations */
    uint64_t *got;                    /* pointer to GOT (from DT_PLTGOT) */
    struct ld_library *next;          /* linked list */
    int      is_main;                 /* 1 if this is the main executable */
};

/* ================================================================
 * Parsed auxiliary vector
 * ================================================================ */
struct ld_auxv {
    uint64_t at_phdr;    /* virtual address of program headers */
    uint64_t at_phent;   /* size of program header entry */
    uint64_t at_phnum;   /* number of program headers */
    uint64_t at_pagesz;  /* page size */
    uint64_t at_entry;   /* entry point of the main executable */
    uint64_t at_base;    /* base address of the interpreter (ld.so) */
};

/* ================================================================
 * Dynamic linker function declarations
 * ================================================================ */
void _start(void);
void ld_parse_auxv(Elf64_auxv_t *auxv, struct ld_auxv *parsed);
int  ld_load_dependencies(struct ld_library *lib, struct ld_library **head);
int  ld_relocate(struct ld_library *lib, struct ld_library *head);
uint64_t ld_bind_plt(struct ld_library *lib, int reloc_idx);
struct ld_library *ld_map_library(const char *path);
int  ld_find_symbol(struct ld_library *head, const char *name,
                     struct ld_library **found_lib, Elf64_Sym **found_sym);
int  ld_resolve_path(const char *name, char *out, int outsz);

#endif /* LD_SO_H */