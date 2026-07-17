/* DobUITools — Text Clipboard Implementation
 *
 * A single SHM region backs the clipboard. Each process maps it on
 * first use and keeps the mapping for the rest of its life — reads
 * and writes then go through the mapping directly, so the hot path
 * (cliptext_size, cliptext_get called every UI refresh) performs
 * zero IPC.
 *
 * The region is resized only when a Set exceeds its capacity. Even
 * then we keep things consistent across processes: create the new
 * region first, publish its id through dobconfig, and only then mark
 * the old region's magic as STALE and drop our mapping. Readers
 * still pointing at the old region notice the stale magic on their
 * next access and re-attach via dobconfig.
 */

#include "cliptext.h"
#include <DobConfig.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLIPTEXT_KEY      "clipboard_text"
#define CLIPTEXT_MAGIC    0x544C4344u   /* 'DCLT' little-endian */
#define CLIPTEXT_STALE    0x454C5453u   /* 'STLE' little-endian */
#define CLIPTEXT_INIT_CAP 4096u
#define CLIPTEXT_SLACK    64u           /* headroom on resize */

typedef struct
{
    uint32_t magic;
    uint32_t seq;
    uint32_t used;
    uint32_t cap;
    char     payload[];
} clip_hdr_t;

static clip_hdr_t *g_hdr = NULL;
static int         g_id  = -1;

/* Local mapping lifecycle */

static void detach(void)
{
    if (g_id >= 0)
    {
        shm_unmap(g_id);
        g_id  = -1;
        g_hdr = NULL;
    }
}

/* Attach to the region whose id is currently published in dobconfig.
 * If `g_hdr` already points at a live region, no-op. Returns true
 * if `g_hdr` is valid on return. */
static bool ensure_attached(void)
{
    if (g_hdr && g_hdr->magic == CLIPTEXT_MAGIC)
        return true;

    /* Our mapping is stale (or absent). Drop it and start over. */
    detach();

    char buf[16];
    if (dobconfig_Get(CLIPTEXT_KEY, buf, sizeof(buf)) != 0)
        return false;
    if (buf[0] == '\0')
        return false;

    int id = atoi(buf);
    if (id < 0) return false;

    uint32_t va = 0;
    if (shm_map(id, &va) != 0) return false;

    clip_hdr_t *hdr = (clip_hdr_t *)va;
    if (hdr->magic != CLIPTEXT_MAGIC)
    {
        shm_unmap(id);
        return false;
    }

    g_id  = id;
    g_hdr = hdr;
    return true;
}

/* Create a fresh SHM region sized for at least `need` payload bytes,
 * publish its id, and swap our mapping to it. On success the prior
 * region (if any) is marked STALE and unmapped. Returns 0 / -1. */
static int rotate_region(uint32_t need)
{
    uint32_t cap = need + CLIPTEXT_SLACK;
    if (cap < CLIPTEXT_INIT_CAP) cap = CLIPTEXT_INIT_CAP;

    uint32_t va = 0;
    int new_id = shm_create(sizeof(clip_hdr_t) + cap, &va);
    if (new_id < 0) return -1;

    clip_hdr_t *nh = (clip_hdr_t *)va;
    nh->magic = CLIPTEXT_MAGIC;
    nh->seq   = g_hdr ? g_hdr->seq + 1 : 1;
    nh->used  = 0;
    nh->cap   = cap;

    char idbuf[16];
    snprintf(idbuf, sizeof(idbuf), "%d", new_id);
    if (dobconfig_Set(CLIPTEXT_KEY, idbuf) != 0)
    {
        shm_unmap(new_id);
        return -1;
    }

    /* Swap: invalidate the old region for late readers, then drop it. */
    if (g_hdr)
    {
        g_hdr->magic = CLIPTEXT_STALE;
        shm_unmap(g_id);
    }
    g_id  = new_id;
    g_hdr = nh;
    return 0;
}

/* Public API */

int dobui_cliptext_set(const char *text, int len)
{
    if (!text) return -1;
    if (len < 0) len = (int)strlen(text);
    if (len < 0) return -1;

    uint32_t need = (uint32_t)len;

    /* Reuse the existing region when the payload fits. */
    if (ensure_attached() && need <= g_hdr->cap)
    {
        if (need > 0) memcpy(g_hdr->payload, text, need);
        g_hdr->used = need;
        g_hdr->seq++;
        return 0;
    }

    if (rotate_region(need) != 0)
        return -1;

    if (need > 0) memcpy(g_hdr->payload, text, need);
    g_hdr->used = need;
    return 0;
}

int dobui_cliptext_get(char *buf, int bufsize, int *out_len)
{
    if (out_len) *out_len = 0;
    if (!buf || bufsize <= 0) return -1;
    buf[0] = '\0';

    if (!ensure_attached()) return -1;
    uint32_t used = g_hdr->used;
    if (used == 0) return -1;

    uint32_t take = used;
    if (take > (uint32_t)(bufsize - 1)) take = (uint32_t)(bufsize - 1);
    memcpy(buf, g_hdr->payload, take);
    buf[take] = '\0';
    if (out_len) *out_len = (int)take;
    return 0;
}

int dobui_cliptext_size(void)
{
    if (!ensure_attached()) return -1;
    return g_hdr->used > 0 ? (int)g_hdr->used : -1;
}

int dobui_cliptext_clear(void)
{
    if (!ensure_attached()) return 0;
    g_hdr->used = 0;
    g_hdr->seq++;
    return 0;
}
