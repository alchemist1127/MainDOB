/* MainDOB libc stdlib.c
 * Real malloc/free/calloc/realloc using sbrk() syscall.
 * First-fit free list allocator with splitting and coalescing. */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Block header prepended to every allocation */
typedef struct block
{
    uint32_t        size;       /* Payload size (excluding header) */
    struct block   *next;       /* Next free block (only valid if free) */
    uint32_t        magic;      /* 0xABCD1234 = allocated, 0xDEAD0000 = free */
} block_t;

#define BLOCK_MAGIC_ALLOC   0xABCD1234
#define BLOCK_MAGIC_FREE    0xDEAD0000
#define HEADER_SIZE         sizeof(block_t)
#define MIN_ALLOC           16  /* Minimum payload to avoid extreme fragmentation */
#define SBRK_ALIGN          4096 /* Grow heap in page increments */

static block_t *free_list = NULL;

/* LOCK DELL'ALLOCATORE — obbligatorio, non opzionale. In MainDOB uno
 * stesso address space puo' ospitare DUE contesti di esecuzione su CPU
 * diverse: il main-thread del processo e un dispatch boomerang (il
 * thread del CHIAMANTE, IRETtato qui dal kernel — modello
 * migrating-thread, vedi bga). free_list e il break sono condivisi:
 * senza lock, una free con trimming (sbrk negativo) sul main mentre
 * una malloc cresce nel dispatch produce un BUCO non mappato nel heap
 * — #PF deterministico a meta' buffer, visto dal ferro come il boot
 * bimodale della GUI (vec 14 err 0x4 cr2 nel range brk).
 *
 * NON un futex: nel contesto boomerang l'identita' kernel del thread
 * e' quella del chiamante, e il futex verrebbe chiavato sul processo
 * SBAGLIATO. Serve atomicita' puramente userspace: xchg su una parola
 * dell'AS condiviso, yield nel giro di attesa (progresso garantito:
 * chi tiene il lock gira su un'altra runqueue e non aspetta mai noi;
 * nessun annidamento — le vie interne non riprendono il lock). */
static volatile uint32_t s_heap_lock = 0;

static void heap_lock(void)
{
    while (__sync_lock_test_and_set(&s_heap_lock, 1) != 0)
    {
        yield();
    }
}

static void heap_unlock(void)
{
    __sync_lock_release(&s_heap_lock);
}
static int heap_initialized = 0;

static void heap_init(void)
{
    if (heap_initialized) return;
    /* Call sbrk(0) to get current break, then grow by initial chunk */
    void *base = sbrk(SBRK_ALIGN);
    if (base == (void *)-1) return;

    block_t *b = (block_t *)base;
    b->size = SBRK_ALIGN - HEADER_SIZE;
    b->next = NULL;
    b->magic = BLOCK_MAGIC_FREE;
    free_list = b;
    heap_initialized = 1;
}

/* Grow heap by at least 'min_size' bytes */
static block_t *heap_grow(uint32_t min_size)
{
    uint32_t total = min_size + HEADER_SIZE;
    if (total < SBRK_ALIGN) total = SBRK_ALIGN;
    /* Round up to page */
    total = (total + SBRK_ALIGN - 1) & ~(SBRK_ALIGN - 1);

    void *p = sbrk((int)total);
    if (p == (void *)-1) return NULL;

    block_t *b = (block_t *)p;
    b->size = total - HEADER_SIZE;
    b->next = NULL;
    b->magic = BLOCK_MAGIC_FREE;

    /* Prepend to free list */
    b->next = free_list;
    free_list = b;
    return b;
}

static void *malloc_unlocked(size_t size)
{
    if (size == 0) return NULL;
    if (!heap_initialized) heap_init();

    /* Align to 8 bytes */
    size = (size + 7) & ~7;
    if (size < MIN_ALLOC) size = MIN_ALLOC;

    /* First-fit search */
    block_t **pp = &free_list;
    block_t *b;

    while ((b = *pp) != NULL)
    {
        if (b->size >= size)
        {
            /* Split if remainder is large enough */
            if (b->size >= size + HEADER_SIZE + MIN_ALLOC)
            {
                block_t *rest = (block_t *)((uint8_t *)b + HEADER_SIZE + size);
                rest->size = b->size - size - HEADER_SIZE;
                rest->next = b->next;
                rest->magic = BLOCK_MAGIC_FREE;

                b->size = size;
                *pp = rest;
            }
            else
            {
                /* Use entire block */
                *pp = b->next;
            }

            b->magic = BLOCK_MAGIC_ALLOC;
            b->next = NULL;
            return (void *)((uint8_t *)b + HEADER_SIZE);
        }
        pp = &b->next;
    }

    /* No free block large enough: grow heap */
    b = heap_grow(size);
    if (!b) return NULL;

    /* Retry: the new block is at front of free list */
    return malloc_unlocked(size);
}

static void free_unlocked(void *ptr)
{
    if (!ptr) return;

    block_t *b = (block_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (b->magic != BLOCK_MAGIC_ALLOC) return;  /* Double free or corruption */

    b->magic = BLOCK_MAGIC_FREE;

    /* Insert into free list (sorted by address for coalescing).
     * `pp` ends up pointing at the slot we'll write `b` into;
     * `prev` is the block whose .next we're updating, or NULL if
     * we're inserting at the head. We track it as we walk so we
     * don't have to re-scan to find prev again later. */
    block_t **pp = &free_list;
    block_t  *prev = NULL;
    while (*pp && *pp < b)
    {
        prev = *pp;
        pp = &(*pp)->next;
    }

    b->next = *pp;
    *pp = b;

    /* Coalesce with next block if adjacent */
    if (b->next &&
        (uint8_t *)b + HEADER_SIZE + b->size == (uint8_t *)b->next)
    {
        b->size += HEADER_SIZE + b->next->size;
        b->next = b->next->next;
    }

    /* Coalesce with previous block if adjacent. We already have prev
     * from the insertion walk — the previous version did a second
     * O(n) scan to recover it. */
    if (prev && (uint8_t *)prev + HEADER_SIZE + prev->size == (uint8_t *)b)
    {
        prev->size += HEADER_SIZE + b->size;
        prev->next = b->next;
        b = prev;
    }

    /* Heap trimming: if the last free block reaches the current break,
     * release those pages back to the kernel via sbrk(-N).
     * Keep at least one page to avoid thrashing.
     *
     * sbrk(0) is a syscall — only call it when there's a chance of
     * trimming. The cheap structural check (block big enough to be
     * worth releasing) gates the syscall. */
    if (b->size + HEADER_SIZE < SBRK_ALIGN * 2) return;

    uint8_t *block_end = (uint8_t *)b + HEADER_SIZE + b->size;
    uint8_t *cur_brk = (uint8_t *)sbrk(0);

    if (block_end == cur_brk)
    {
        /* Release all but one page worth */
        uint32_t release = b->size + HEADER_SIZE - SBRK_ALIGN;
        release &= ~(SBRK_ALIGN - 1);  /* Page-align */

        if (release > 0)
        {
            b->size -= release;
            sbrk(-(int)release);
        }
    }
}

void *malloc(size_t size)
{
    heap_lock();
    void *p = malloc_unlocked(size);
    heap_unlock();
    return p;
}

void free(void *ptr)
{
    heap_lock();
    free_unlocked(ptr);
    heap_unlock();
}

void *calloc(size_t count, size_t size)
{
    /* Overflow check: if count * size wraps, malloc would allocate
     * too little memory and memset would corrupt the heap. */
    if (count != 0 && size > UINT32_MAX / count)
        return NULL;
    size_t total = count * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    /* Tutto sotto il lock, con le vie interne: header, copia e free
     * del vecchio blocco devono essere atomici rispetto all'altro
     * contesto (una free concorrente potrebbe fondere/spostare b). */
    heap_lock();
    block_t *b = (block_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (b->magic != BLOCK_MAGIC_ALLOC)
    {
        heap_unlock();
        return NULL;
    }
    if (b->size >= size)
    {
        heap_unlock();
        return ptr;                     /* Already large enough */
    }
    void *newp = malloc_unlocked(size);
    if (newp)
    {
        memcpy(newp, ptr, b->size);
        free_unlocked(ptr);
    }
    heap_unlock();
    return newp;
}

int atoi(const char *s)
{
    int result = 0;
    int sign = 1;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    while (*s >= '0' && *s <= '9')
    {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result * sign;
}

void abort(void)
{
    debug_print("abort() called\n");
    _exit(127);
}

void exit(int code)
{
    _exit(code);
}

long
strtol(const char *s, char **endp, int base)
{
    long result = 0;
    int sign = 1;
    int digit;

    while (*s == ' ' || *s == '\t')
        s++;

    if (*s == '-')      { sign = -1; s++; }
    else if (*s == '+') { s++; }

    if (base == 0)
    {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X'))
            { base = 16; s += 2; }
        else if (*s == '0')
            { base = 8; s++; }
        else
            { base = 10; }
    }
    else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        s += 2;
    }

    while (*s)
    {
        if (*s >= '0' && *s <= '9')      digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;

        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }

    if (endp) *endp = (char *)s;
    return result * sign;
}

long
atol(const char *s)
{
    return strtol(s, NULL, 10);
}

int
abs(int x)
{
    return x < 0 ? -x : x;
}

long
labs(long x)
{
    return x < 0 ? -x : x;
}
