/* DobWrite -- font registry.
 *
 * Holds the set of installed fonts (builtin + loaded from files), each a
 * df_face opened via df_open (TTF/OTF outline or DMF1 monobit, picked
 * automatically by the backend).  The font dropdown and the fontset that
 * the layout uses are both built from this registry, so adding a font
 * file is the only thing needed to make a new family available -- nothing
 * is hardcoded in the layout path.
 */
#ifndef DOBWRITE_FONTREG_H
#define DOBWRITE_FONTREG_H

#include <dobfont/dobfont.h>
#include <dobfont/resolver.h>

#define FONTREG_MAX   32
#define FONTREG_NAME  64

typedef struct
{
    char      name[FONTREG_NAME];
    df_face  *face;
    void     *blob;        /* the font bytes df_open keeps a pointer into */
    uint32_t  blob_len;
    bool      owns_blob;   /* true => fontreg_free() frees blob           */
} fontreg_entry;

typedef struct
{
    fontreg_entry e[FONTREG_MAX];
    int           n;
} fontreg_t;

void        fontreg_init(fontreg_t *r);

/* Open `blob` and register it under `name`.  df_open auto-detects the
 * format.  If take_ownership, the registry frees the blob in
 * fontreg_free; otherwise the caller keeps ownership (e.g. a static
 * builtin blob).  A duplicate name is rejected.  Returns the entry index,
 * or -1 if not added (the caller still owns the blob in that case). */
int         fontreg_add(fontreg_t *r, const char *name,
                        void *blob, uint32_t blob_len, bool take_ownership);

int         fontreg_count(const fontreg_t *r);
const char *fontreg_name (const fontreg_t *r, int i);
df_face    *fontreg_face (const fontreg_t *r, int i);
int         fontreg_find (const fontreg_t *r, const char *name);   /* -1 if none */

/* Build a fontset with every registered font added under its name. The
 * caller owns the returned fontset (df_fontset_destroy). NULL on failure. */
df_fontset *fontreg_build_fontset(const fontreg_t *r);

/* Close every face and free owned blobs.  After this the registry is
 * empty.  Destroy any fontset built from it BEFORE calling this. */
void        fontreg_free(fontreg_t *r);

#endif /* DOBWRITE_FONTREG_H */
