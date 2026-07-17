#include "lib/string.h"

/* Implementazione osservata dall'1.0 (lib/string.c) e conservata dove
 * gia' ottima: i fast-path `rep stosl`/`rep movsl` sono il modo giusto
 * di muovere memoria su P6/Core2 (Armada E500, Extensa 5220) — il
 * microcodice li ottimizza da solo. Cambia solo l'organizzazione:
 * i verbi comuni (copia dword avanti, coda a byte) sono fattorizzati
 * una volta invece che ripetuti in memcpy e nei due rami di memmove. */

/* === Verbi interni ===================================================== */

/* Copia in avanti: dword con rep movsl, coda a byte. dst/src NON devono
 * sovrapporsi in avanti (garanzia del chiamante). */
static void copy_forward(uint8_t *dst, const uint8_t *src, size_t n)
{
    void *d = dst;
    void *s = (void *)src;
    uint32_t dwords = n >> 2;
    uint32_t trail  = n & 3;

    __asm__ volatile (
        "rep movsl"
        : "+D"(d), "+S"(s), "+c"(dwords)
        :
        : "memory"
    );

    uint8_t *db = (uint8_t *)d;
    const uint8_t *sb = (const uint8_t *)s;
    while (trail--)
    {
        *db++ = *sb++;
    }
}

/* Copia all'indietro per regioni sovrapposte (dst > src): prima la coda
 * a byte dal fondo, poi le dword con `std; rep movsl; cld`. */
static void copy_backward(uint8_t *dst, const uint8_t *src, size_t n)
{
    uint8_t *dt = dst + n;
    const uint8_t *st = src + n;
    uint32_t dwords = n >> 2;
    uint32_t trail  = n & 3;

    while (trail--)
    {
        *--dt = *--st;
    }

    if (dwords > 0)
    {
        void *d_end = dst + (n & ~3u) - 4;
        void *s_end = (void *)(src + (n & ~3u) - 4);
        __asm__ volatile (
            "std; rep movsl; cld"
            : "+D"(d_end), "+S"(s_end), "+c"(dwords)
            :
            : "memory"
        );
    }
}

/* === API standard ====================================================== */

void *memset(void *dst, int val, size_t n)
{
    void *ret = dst;
    uint8_t byte = (uint8_t)val;
    uint32_t dword = (uint32_t)byte | ((uint32_t)byte << 8)
                   | ((uint32_t)byte << 16) | ((uint32_t)byte << 24);
    uint32_t dwords = n >> 2;
    uint32_t trail  = n & 3;

    __asm__ volatile (
        "rep stosl"
        : "+D"(dst), "+c"(dwords)
        : "a"(dword)
        : "memory"
    );

    uint8_t *d = (uint8_t *)dst;
    while (trail--)
    {
        *d++ = byte;
    }

    return ret;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    copy_forward((uint8_t *)dst, (const uint8_t *)src, n);
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    if (dst < src || (uint8_t *)dst >= (const uint8_t *)src + n)
    {
        copy_forward((uint8_t *)dst, (const uint8_t *)src, n);
    }
    else if (dst > src)
    {
        copy_backward((uint8_t *)dst, (const uint8_t *)src, n);
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++)
    {
        if (pa[i] != pb[i])
        {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len])
    {
        len++;
    }
    return len;
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t len = 0;
    while (len < maxlen && s[len])
    {
        len++;
    }
    return len;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (a[i] != b[i])
        {
            return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        }
        if (a[i] == '\0')
        {
            break;
        }
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
    {
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
    {
        dst[i] = src[i];
    }
    for (; i < n; i++)
    {
        dst[i] = '\0';
    }
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t src_len = strlen(src);

    if (size > 0)
    {
        size_t copy = (src_len < size - 1) ? src_len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return src_len;
}

char *strstr(const char *haystack, const char *needle)
{
    if (*needle == '\0')
    {
        return (char *)haystack;
    }

    for (; *haystack != '\0'; haystack++)
    {
        const char *h = haystack;
        const char *n = needle;
        while (*h != '\0' && *n != '\0' && *h == *n)
        {
            h++;
            n++;
        }
        if (*n == '\0')
        {
            return (char *)haystack;
        }
    }
    return NULL;
}
