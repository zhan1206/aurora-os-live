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
    if (*s == '-') { sign = -1; s++; }
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
                buf[n++] = (char)__builtin_va_arg(ap, int);
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
