/* Userspace libc with syscall wrappers, malloc/free, and formatting */

/* ================================================================
 * Syscall wrappers
 * ================================================================ */
static inline long sys_call(long num, long a1, long a2, long a3) {
    long ret;
    asm volatile (
        "mov %1, %%rax\n\t"
        "mov %2, %%rdi\n\t"
        "mov %3, %%rsi\n\t"
        "mov %4, %%rdx\n\t"
        "syscall\n\t"
        "mov %%rax, %0\n\t"
        : "=r" (ret)
        : "r" (num), "r" (a1), "r" (a2), "r" (a3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

int write(int fd, const void *buf, unsigned long count) {
    return (int)sys_call(1, fd, (long)buf, (long)count);
}

int read(int fd, void *buf, unsigned long count) {
    return (int)sys_call(0, fd, (long)buf, (long)count);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)sys_call(59, (long)path, (long)argv, (long)envp);
}

int getpid(void) {
    return (int)sys_call(39, 0, 0, 0);
}

void exit(int code) {
    sys_call(60, code, 0, 0);
    for (;;) ;  /* unreachable */
}

int fork(void) {
    return (int)sys_call(57, 0, 0, 0);
}

int waitpid(int pid, int *status, int options) {
    return (int)sys_call(61, pid, (long)status, options);
}

/* ================================================================
 * String utilities
 * ================================================================ */
static unsigned long strlen(const char *s) {
    unsigned long n = 0; while (s[n]) ++n; return n;
}

static int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

static int strncmp(const char *a, const char *b, unsigned long n) {
    if (!n) return 0;
    while (--n && *a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

static void *memset(void *s, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static void *memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ================================================================
 * Number formatting
 * ================================================================ */
static int itoa_int(int val, char *buf, int bufsz) {
    int n = 0;
    /* Bug #43: val = -val is UB when val == INT_MIN.
     * Handle INT_MIN as a special case by using unsigned arithmetic. */
    if (val < 0) {
        if (n < bufsz-1) buf[n++] = '-';
        unsigned int uval = (unsigned int)(-(val + 1)) + 1U;
        char tmp[16]; int tn = 0;
        if (uval == 0) tmp[tn++] = '0';
        while (uval > 0 && tn < 15) { tmp[tn++] = '0' + (uval % 10); uval /= 10; }
        for (int i = tn - 1; i >= 0 && n < bufsz-1; i--) buf[n++] = tmp[i];
        buf[n] = '\0';
        return n;
    }
    if (val == 0) { if (n < bufsz-1) buf[n++] = '0'; buf[n] = '\0'; return n; }
    char tmp[16]; int tn = 0;
    while (val > 0 && tn < 15) { tmp[tn++] = '0' + (val % 10); val /= 10; }
    for (int i = tn - 1; i >= 0 && n < bufsz-1; i--) buf[n++] = tmp[i];
    buf[n] = '\0';
    return n;
}

int atoi(const char *s) {
    int v = 0, sign = 1;
    while (*s == ' ') s++;
    /* FIXED (v4.1.8): Handle '+' sign (M-5) */
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}

/* ================================================================
 * Formatted output
 * ================================================================ */
int puts(const char *s) {
    unsigned long n = strlen(s);
    write(1, s, n);
    write(1, "\n", 1);
    return 0;
}

int printf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    char buf[512];
    int n = 0;
    const char *p = fmt;
    while (*p && n < 500) {
        if (*p == '%') {
            p++;
            if (*p == 's') {
                const char *s = __builtin_va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s && n < 500) buf[n++] = *s++;
            } else if (*p == 'd' || *p == 'i') {
                int v = __builtin_va_arg(ap, int);
                char tmp[16];
                int tn = itoa_int(v, tmp, 16);
                for (int i = 0; i < tn && n < 500; i++) buf[n++] = tmp[i];
            } else if (*p == 'u') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                char tmp[16]; int tn = 0;
                if (v == 0) tmp[tn++] = '0';
                while (v > 0 && tn < 15) { tmp[tn++] = '0' + (v % 10); v /= 10; }
                for (int i = tn-1; i >= 0 && n < 500; i--) buf[n++] = tmp[i];
            } else if (*p == 'c') {
                char c = (char)__builtin_va_arg(ap, int);
                /* Bug #42: bounds check before writing to buf */
                if (n < 500) buf[n++] = c;
            } else if (*p == 'x') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                char tmp[16]; int tn = 0;
                if (v == 0) tmp[tn++] = '0';
                while (v > 0 && tn < 15) { int nib = v & 0xF; tmp[tn++] = nib < 10 ? '0'+nib : 'a'+nib-10; v >>= 4; }
                /* Bug #42: bounds check for "0x" prefix */
                if (n < 500) buf[n++] = '0';
                if (n < 500) buf[n++] = 'x';
                for (int i = tn-1; i >= 0 && n < 500; i--) buf[n++] = tmp[i];
            } else {
                buf[n++] = '%'; if (*p) buf[n++] = *p;
            }
            if (*p) p++;
        } else {
            buf[n++] = *p++;
        }
    }
    buf[n] = '\0';
    __builtin_va_end(ap);
    if (n > 0) write(1, buf, (unsigned long)n);
    return n;
}

/*
 * FIXED (v4.1.8): Added snprintf with explicit buffer size parameter.
 * sprintf remains for backward compatibility but is limited to 500 chars.
 * (M-2: sprintf no buffer size parameter)
 */
int snprintf(char *buf, size_t size, const char *fmt, ...) {
    if (!buf || size == 0) return 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = 0;
    const char *p = fmt;
    size_t limit = size - 1;  /* reserve 1 byte for null terminator */
    while (*p && n < (int)limit) {
        if (*p == '%') {
            p++;
            if (*p == 's') {
                const char *s = __builtin_va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s && n < (int)limit) buf[n++] = *s++;
            } else if (*p == 'd' || *p == 'i') {
                int v = __builtin_va_arg(ap, int);
                char tmp[16];
                int tn = itoa_int(v, tmp, 16);
                for (int i = 0; i < tn && n < (int)limit; i++) buf[n++] = tmp[i];
            } else if (*p == 'u') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                char tmp[16]; int tn = 0;
                if (v == 0) tmp[tn++] = '0';
                while (v > 0 && tn < 15) { tmp[tn++] = '0' + (v % 10); v /= 10; }
                for (int i = tn-1; i >= 0 && n < (int)limit; i--) buf[n++] = tmp[i];
            } else if (*p == 'c') {
                if (n < (int)limit) buf[n++] = (char)__builtin_va_arg(ap, int);
            } else if (*p == 'x') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                char tmp[16]; int tn = 0;
                if (v == 0) tmp[tn++] = '0';
                while (v > 0 && tn < 15) { int nib = v & 0xF; tmp[tn++] = nib < 10 ? '0'+nib : 'a'+nib-10; v >>= 4; }
                if (n < (int)limit) buf[n++] = '0';
                if (n < (int)limit) buf[n++] = 'x';
                for (int i = tn-1; i >= 0 && n < (int)limit; i--) buf[n++] = tmp[i];
            } else {
                if (n < (int)limit) buf[n++] = '%';
                if (*p && n < (int)limit) buf[n++] = *p;
            }
            if (*p) p++;
        } else {
            buf[n++] = *p++;
        }
    }
    buf[n] = '\0';
    __builtin_va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = 0;
    const char *p = fmt;
    while (*p && n < 500) {
        if (*p == '%') {
            p++;
            if (*p == 's') {
                const char *s = __builtin_va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s && n < 500) buf[n++] = *s++;
            } else if (*p == 'd') {
                int v = __builtin_va_arg(ap, int);
                char tmp[16];
                int tn = itoa_int(v, tmp, 16);
                for (int i = 0; i < tn && n < 500; i++) buf[n++] = tmp[i];
            } else if (*p == 'c') {
                if (n < 500) buf[n++] = (char)__builtin_va_arg(ap, int);
            } else if (*p == 'u') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                char tmp[16]; int tn = 0;
                if (v == 0) tmp[tn++] = '0';
                while (v > 0 && tn < 15) { tmp[tn++] = '0' + (v % 10); v /= 10; }
                for (int i = tn-1; i >= 0 && n < 500; i--) buf[n++] = tmp[i];
            } else if (*p == 'x') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                char tmp[16]; int tn = 0;
                if (v == 0) tmp[tn++] = '0';
                while (v > 0 && tn < 15) { int nib = v & 0xF; tmp[tn++] = nib < 10 ? '0'+nib : 'a'+nib-10; v >>= 4; }
                if (n < 500) buf[n++] = '0';
                if (n < 500) buf[n++] = 'x';
                for (int i = tn-1; i >= 0 && n < 500; i--) buf[n++] = tmp[i];
            } else if (*p == 'i') {
                int v = __builtin_va_arg(ap, int);
                char tmp[16];
                int tn = itoa_int(v, tmp, 16);
                for (int i = 0; i < tn && n < 500; i++) buf[n++] = tmp[i];
            } else {
                buf[n++] = '%'; if (*p) buf[n++] = *p;
            }
            if (*p) p++;
        } else {
            buf[n++] = *p++;
        }
    }
    buf[n] = '\0';
    __builtin_va_end(ap);
    return n;
}

/* ================================================================
 * Simple malloc/free using sbrk (bump allocator for user space)
 * ================================================================ */
#define HEAP_CHUNK_SIZE 4096

typedef struct heap_block {
    unsigned long size;     /* total block size including header */
    int           free;     /* 1 if free, 0 if used */
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

static heap_block_t *heap_head = NULL;
static int heap_initialized = 0;

/* Minimal sbrk: use a fixed user-space heap area */
#define USER_HEAP_START  ((char *)0x600000)
#define USER_HEAP_END    ((char *)0x700000)
static char *heap_brk = USER_HEAP_START;

static void *sbrk(long increment) {
    char *old = heap_brk;
    if (heap_brk + increment > USER_HEAP_END) return (void *)-1;
    heap_brk += increment;
    return old;
}

static void heap_init(void) {
    if (heap_initialized) return;
    heap_initialized = 1;
    void *mem = sbrk(HEAP_CHUNK_SIZE);
    if (mem == (void *)-1) return;
    heap_block_t *block = (heap_block_t *)mem;
    block->size = HEAP_CHUNK_SIZE;
    block->free = 1;
    block->next = NULL;
    block->prev = NULL;
    heap_head = block;
}

void *malloc(unsigned long size) {
    if (!heap_initialized) heap_init();
    if (!heap_head || size == 0) return NULL;

    /* Align size to 8 bytes */
    size = (size + 7) & ~7UL;
    unsigned long needed = size + sizeof(heap_block_t);

    heap_block_t *block = heap_head;
    while (block) {
        if (block->free && block->size >= needed) {
            /* Split if large enough */
            if (block->size >= needed + sizeof(heap_block_t) + 16) {
                heap_block_t *new_block = (heap_block_t *)((char *)block + needed);
                new_block->size = block->size - needed;
                new_block->free = 1;
                new_block->next = block->next;
                new_block->prev = block;
                if (block->next) block->next->prev = new_block;
                block->next = new_block;
                block->size = needed;
            }
            block->free = 0;
            return (void *)((char *)block + sizeof(heap_block_t));
        }
        block = block->next;
    }

    /* No suitable block, expand heap */
    unsigned long expand = needed > HEAP_CHUNK_SIZE ? needed : HEAP_CHUNK_SIZE;
    void *mem = sbrk((long)expand);
    if (mem == (void *)-1) return NULL;

    heap_block_t *new_block = (heap_block_t *)mem;
    new_block->size = expand;
    new_block->free = 0;
    new_block->next = NULL;

    /* Append to list */
    if (!heap_head) {
        new_block->prev = NULL;
        heap_head = new_block;
    } else {
        heap_block_t *tail = heap_head;
        while (tail->next) tail = tail->next;
        tail->next = new_block;
        new_block->prev = tail;
    }

    return (void *)((char *)new_block + sizeof(heap_block_t));
}

void free(void *ptr) {
    if (!ptr || !heap_head) return;
    heap_block_t *block = (heap_block_t *)((char *)ptr - sizeof(heap_block_t));
    block->free = 1;

    /* Coalesce with next block */
    if (block->next && block->next->free) {
        block->size += block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }

    /* Coalesce with previous block */
    if (block->prev && block->prev->free) {
        block->prev->size += block->size;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
    }
}

void *calloc(unsigned long nmemb, unsigned long size) {
    /* Bug #41: nmemb * size can overflow, leading to a small allocation
     * and a memset that writes past the buffer. Check for overflow. */
    if (nmemb != 0 && size > (~0UL) / nmemb) return NULL;
    unsigned long total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, unsigned long size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    heap_block_t *block = (heap_block_t *)((char *)ptr - sizeof(heap_block_t));
    unsigned long old_size = block->size - sizeof(heap_block_t);
    if (size <= old_size) return ptr;
    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}

/* ================================================================
 * FIXED (v4.3.8): LIBC-001 — Standard I/O: FILE, fopen/fclose/fread/fwrite
 * ================================================================ */
typedef struct {
    int   fd;
    int   eof;
    int   error;
    char *buf;
    unsigned long buf_size;
    unsigned long buf_pos;
    unsigned long buf_len;
} FILE;

#define EOF (-1)
#define BUFSIZ 4096

static FILE _stdin  = {0, 0, 0, NULL, 0, 0, 0};
static FILE _stdout = {1, 0, 0, NULL, 0, 0, 0};
static FILE _stderr = {2, 0, 0, NULL, 0, 0, 0};

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

FILE *fopen(const char *path, const char *mode) {
    int flags = 0;
    if (mode[0] == 'r') flags = 0;
    else if (mode[0] == 'w') flags = 0;  /* write/create */
    else if (mode[0] == 'a') flags = 0;  /* append */
    else return NULL;

    int fd = (int)sys_call(2, (long)path, (long)flags, 0);  /* SYS_OPEN */
    if (fd < 0) return NULL;

    FILE *fp = (FILE *)malloc(sizeof(FILE));
    if (!fp) { sys_call(3, fd, 0, 0); return NULL; }  /* SYS_CLOSE */
    fp->fd = fd;
    fp->eof = 0;
    fp->error = 0;
    fp->buf = (char *)malloc(BUFSIZ);
    fp->buf_size = fp->buf ? BUFSIZ : 0;
    fp->buf_pos = 0;
    fp->buf_len = 0;
    return fp;
}

int fclose(FILE *fp) {
    if (!fp) return EOF;
    int ret = (int)sys_call(3, fp->fd, 0, 0);  /* SYS_CLOSE */
    if (fp->buf) free(fp->buf);
    free(fp);
    return ret;
}

unsigned long fread(void *ptr, unsigned long size, unsigned long nmemb, FILE *fp) {
    if (!fp || !ptr || size == 0 || nmemb == 0) return 0;
    unsigned long total = size * nmemb;
    unsigned long read_total = 0;
    char *dst = (char *)ptr;

    while (read_total < total) {
        /* Read directly from fd */
        long n = sys_call(0, fp->fd, (long)(dst + read_total), (long)(total - read_total));
        if (n <= 0) { fp->eof = (n == 0); fp->error = (n < 0); break; }
        read_total += (unsigned long)n;
    }
    return read_total / size;
}

unsigned long fwrite(const void *ptr, unsigned long size, unsigned long nmemb, FILE *fp) {
    if (!fp || !ptr || size == 0 || nmemb == 0) return 0;
    unsigned long total = size * nmemb;
    unsigned long written = 0;
    const char *src = (const char *)ptr;

    while (written < total) {
        long n = sys_call(1, fp->fd, (long)(src + written), (long)(total - written));
        if (n <= 0) { fp->error = 1; break; }
        written += (unsigned long)n;
    }
    return written / size;
}

int fgetc(FILE *fp) {
    unsigned char c;
    if (fread(&c, 1, 1, fp) != 1) return EOF;
    return (int)c;
}

int fputc(int c, FILE *fp) {
    unsigned char ch = (unsigned char)c;
    if (fwrite(&ch, 1, 1, fp) != 1) return EOF;
    return c;
}

/* ================================================================
 * FIXED (v4.3.8): LIBC-002 — getenv/setenv using syscalls
 * ================================================================ */
char *getenv(const char *name) {
    /* SYS_GETENV: returns a pointer to a static buffer with the value */
    long ret = sys_call(257, (long)name, 0, 0);  /* SYS_GETENV */
    if (ret <= 0) return NULL;
    return (char *)ret;
}

int setenv(const char *name, const char *value, int overwrite) {
    /* SYS_SETENV */
    return (int)sys_call(325, (long)name, (long)value, (long)overwrite);
}

/* ================================================================
 * FIXED (v4.3.8): LIBC-003 — strtok: tokenize string by delimiters
 * ================================================================ */
char *strtok(char *str, const char *delim) {
    static char *saved = NULL;
    if (str) saved = str;
    if (!saved) return NULL;

    /* Skip leading delimiters */
    while (*saved) {
        int is_delim = 0;
        for (const char *d = delim; *d; d++) {
            if (*saved == *d) { is_delim = 1; break; }
        }
        if (!is_delim) break;
        saved++;
    }
    if (*saved == '\0') return NULL;

    char *token = saved;
    /* Find end of token */
    while (*saved) {
        int is_delim = 0;
        for (const char *d = delim; *d; d++) {
            if (*saved == *d) { is_delim = 1; break; }
        }
        if (is_delim) {
            *saved = '\0';
            saved++;
            return token;
        }
        saved++;
    }
    /* End of string: last token, no more calls */
    saved = NULL;
    return token;
}

/* ================================================================
 * FIXED (v4.3.8): LIBC-004 — strcpy/strncpy string copy functions
 * ================================================================ */
char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, unsigned long n) {
    unsigned long i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}
