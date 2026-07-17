/* DobWrite -- font registry implementation. See fontreg.h. */

#include "fontreg.h"
#include <string.h>
#include <stdlib.h>

void fontreg_init(fontreg_t *r) { if (r) memset(r, 0, sizeof(*r)); }

static void copy_name(char *dst, const char *src)
{
    size_t i = 0;
    if (src) for (; src[i] && i < FONTREG_NAME - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

int fontreg_count(const fontreg_t *r)            { return r ? r->n : 0; }
const char *fontreg_name(const fontreg_t *r, int i){ return (r && i >= 0 && i < r->n) ? r->e[i].name : NULL; }
df_face    *fontreg_face(const fontreg_t *r, int i){ return (r && i >= 0 && i < r->n) ? r->e[i].face : NULL; }

int fontreg_find(const fontreg_t *r, const char *name)
{
    if (!r || !name) return -1;
    for (int i = 0; i < r->n; i++)
        if (strcmp(r->e[i].name, name) == 0) return i;
    return -1;
}

int fontreg_add(fontreg_t *r, const char *name, void *blob,
                uint32_t blob_len, bool take_ownership)
{
    if (!r || !name || !blob || blob_len == 0 || r->n >= FONTREG_MAX) return -1;
    if (fontreg_find(r, name) >= 0) return -1;          /* no duplicates */

    df_face *face = NULL;
    if (df_open(blob, blob_len, 0, &face) != DF_OK || !face) return -1;

    fontreg_entry *en = &r->e[r->n];
    copy_name(en->name, name);
    en->face       = face;
    en->blob       = blob;
    en->blob_len   = blob_len;
    en->owns_blob  = take_ownership;
    return r->n++;
}

df_fontset *fontreg_build_fontset(const fontreg_t *r)
{
    if (!r) return NULL;
    df_fontset *set = df_fontset_create();
    if (!set) return NULL;
    for (int i = 0; i < r->n; i++)
        df_fontset_add(set, r->e[i].name, r->e[i].face, "(reg)", 0);
    return set;
}

void fontreg_free(fontreg_t *r)
{
    if (!r) return;
    for (int i = 0; i < r->n; i++)
    {
        if (r->e[i].face) df_close(r->e[i].face);
        if (r->e[i].owns_blob && r->e[i].blob) free(r->e[i].blob);
    }
    memset(r, 0, sizeof(*r));
}
