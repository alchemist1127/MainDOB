#include <string.h>

void *
memset(void *dst, int val, size_t n)
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
        *d++ = byte;

    return ret;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
    void *ret = dst;
    void *src_nc = (void *)src;  /* non-const for asm — rep movsl modifies esi */
    uint32_t dwords = n >> 2;
    uint32_t trail  = n & 3;

    __asm__ volatile (
        "rep movsl"
        : "+D"(dst), "+S"(src_nc), "+c"(dwords)
        :
        : "memory"
    );

    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src_nc;
    while (trail--)
        *d++ = *s++;

    return ret;
}

void *
memmove(void *dst, const void *src, size_t n)
{
    void *ret = dst;

    if (dst < src || (uint8_t *)dst >= (const uint8_t *)src + n)
    {
        void *src_nc = (void *)src;
        uint32_t dwords = n >> 2;
        uint32_t trail  = n & 3;
        __asm__ volatile (
            "rep movsl"
            : "+D"(dst), "+S"(src_nc), "+c"(dwords)
            :
            : "memory"
        );
        uint8_t *d = (uint8_t *)dst;
        const uint8_t *s = (const uint8_t *)src_nc;
        while (trail--)
            *d++ = *s++;
    }
    else if (dst > src)
    {
        uint8_t *dt = (uint8_t *)dst + n;
        const uint8_t *st = (const uint8_t *)src + n;
        uint32_t trail = n & 3;
        while (trail--)
            *--dt = *--st;

        uint32_t dwords = n >> 2;
        if (dwords > 0)
        {
            void *d_end = (uint8_t *)dst + (n & ~3u) - 4;
            void *s_end = (uint8_t *)src + (n & ~3u) - 4;
            __asm__ volatile (
                "std; rep movsl; cld"
                : "+D"(d_end), "+S"(s_end), "+c"(dwords)
                :
                : "memory"
            );
        }
    }

    return ret;
}

int
memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    for (size_t i = 0; i < n; i++)
    {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

size_t
strlen(const char *s)
{
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

int
strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (a[i] != b[i])
            return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        if (a[i] == '\0')
            break;
    }
    return 0;
}

char *
strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *
strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

char *
strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *
strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        d[i] = src[i];
    d[i] = '\0';
    return dst;
}

char *
strchr(const char *s, int c)
{
    while (*s)
    {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *
strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s)
    {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if (c == '\0')
        return (char *)s;
    return (char *)last;
}

char *
strstr(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return (char *)haystack;

    while (*haystack)
    {
        if (strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}
