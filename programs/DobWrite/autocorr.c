/* autocorr.c -- see autocorr.h. */
#include "autocorr.h"
#include <DobFileSystem.h>

static char g_from[AC_MAX_ENTRIES][AC_FROM_CAP];
static char g_to  [AC_MAX_ENTRIES][AC_TO_CAP];
static int  g_n;

static int  ac_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void ac_copy(char *dst, int cap, const char *src)
{
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* A byte that belongs to a "word": ASCII alphanumeric, or any byte >= 0x80
 * (UTF-8 lead/continuation), so accented letters count as word characters. */
static bool ac_is_word_byte(unsigned char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           c >= 0x80;
}

bool autocorr_add(const char *from, const char *to)
{
    if (!from || !to || !from[0] || g_n >= AC_MAX_ENTRIES) return false;
    if (ac_strlen(from) >= AC_FROM_CAP || ac_strlen(to) >= AC_TO_CAP) return false;
    ac_copy(g_from[g_n], AC_FROM_CAP, from);
    ac_copy(g_to[g_n],   AC_TO_CAP,   to);
    g_n++;
    return true;
}

void autocorr_remove(int i)
{
    if (i < 0 || i >= g_n) return;
    for (int k = i; k < g_n - 1; k++) {
        ac_copy(g_from[k], AC_FROM_CAP, g_from[k + 1]);
        ac_copy(g_to[k],   AC_TO_CAP,   g_to[k + 1]);
    }
    g_n--;
}

int         autocorr_count(void) { return g_n; }
const char *autocorr_from(int i) { return (i >= 0 && i < g_n) ? g_from[i] : 0; }
const char *autocorr_to(int i)   { return (i >= 0 && i < g_n) ? g_to[i]   : 0; }

static void ac_put_u16(uint8_t *b, int *p, unsigned v)
{ b[(*p)++] = (uint8_t)(v & 0xFF); b[(*p)++] = (uint8_t)((v >> 8) & 0xFF); }
static unsigned ac_get_u16(const uint8_t *b, int *p)
{ unsigned v = (unsigned)b[*p] | ((unsigned)b[*p + 1] << 8); *p += 2; return v; }

/* On-disk format: "DACR" + u16 count, then per entry u16 from_len, from bytes,
 * u16 to_len, to bytes.  Buffers are static (the worst case is ~10 KB). */
void autocorr_save(const char *path)
{
    static uint8_t buf[8 + AC_MAX_ENTRIES * (4 + AC_FROM_CAP + AC_TO_CAP)];
    int p = 0;
    buf[p++] = 'D'; buf[p++] = 'A'; buf[p++] = 'C'; buf[p++] = 'R';
    ac_put_u16(buf, &p, (unsigned)g_n);
    for (int i = 0; i < g_n; i++) {
        int fl = ac_strlen(g_from[i]), tl = ac_strlen(g_to[i]);
        ac_put_u16(buf, &p, (unsigned)fl);
        for (int k = 0; k < fl; k++) buf[p++] = (uint8_t)g_from[i][k];
        ac_put_u16(buf, &p, (unsigned)tl);
        for (int k = 0; k < tl; k++) buf[p++] = (uint8_t)g_to[i][k];
    }
    int fd = dobfs_Open(path, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0) return;
    dobfs_Write(fd, buf, (uint32_t)p);
    dobfs_Close(fd);
}

void autocorr_load(const char *path)
{
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) return;                        /* no config yet: keep current list */
    static uint8_t buf[8 + AC_MAX_ENTRIES * (4 + AC_FROM_CAP + AC_TO_CAP)];
    int len = 0, n;
    while (len < (int)sizeof buf &&
           (n = dobfs_Read(fd, buf + len, (uint32_t)((int)sizeof buf - len))) > 0)
        len += n;
    dobfs_Close(fd);
    if (len < 6 || buf[0] != 'D' || buf[1] != 'A' || buf[2] != 'C' || buf[3] != 'R')
        return;                                /* missing/invalid: keep current list */
    int p = 4;
    unsigned cnt = ac_get_u16(buf, &p);
    g_n = 0;                                    /* replace the list with the saved one */
    for (unsigned i = 0; i < cnt && g_n < AC_MAX_ENTRIES; i++) {
        if (p + 2 > len) break;
        unsigned fl = ac_get_u16(buf, &p);
        if (p + (int)fl > len || fl >= AC_FROM_CAP) break;
        char from[AC_FROM_CAP];
        for (unsigned k = 0; k < fl; k++) from[k] = (char)buf[p++];
        from[fl] = 0;
        if (p + 2 > len) break;
        unsigned tl = ac_get_u16(buf, &p);
        if (p + (int)tl > len || tl >= AC_TO_CAP) break;
        char to[AC_TO_CAP];
        for (unsigned k = 0; k < tl; k++) to[k] = (char)buf[p++];
        to[tl] = 0;
        autocorr_add(from, to);
    }
}

void autocorr_init(void)
{
    g_n = 0;
    autocorr_add("teh", "the");
    autocorr_add("adn", "and");
    autocorr_add("recieve", "receive");
    autocorr_add("cmq", "comunque");
    autocorr_add("perch\xC3\xA8", "perch\xC3\xA9");   /* perche` -> perche' (grave->acute) */
    autocorr_add("p\xC3\xB2", "po'");                  /* po`     -> po'                    */
}

bool autocorr_match(const char *pre, int pre_len, int *match_len, const char **to)
{
    int best = -1, best_len = 0;
    for (int i = 0; i < g_n; i++) {
        int fl = ac_strlen(g_from[i]);
        if (fl == 0 || fl > pre_len) continue;

        int off = pre_len - fl;            /* candidate start inside pre */
        int j = 0;
        for (; j < fl; j++) if (pre[off + j] != g_from[i][j]) break;
        if (j != fl) continue;             /* tail does not equal this "from" */

        /* must sit on a word boundary: start of buffer, or a non-word byte
         * immediately before it (so "teh" inside "stehp" does not fire). */
        if (off > 0 && ac_is_word_byte((unsigned char)pre[off - 1])) continue;

        if (fl > best_len) { best_len = fl; best = i; }
    }
    if (best < 0) return false;
    *match_len = best_len;
    *to        = g_to[best];
    return true;
}
