/* libdobfont -- font resolver.
 *
 * The policy layer that turns a request -- "family X, weight 700,
 * italic, must contain code point U+20AC" -- into a concrete face plus
 * any synthetic styling to apply. It is the in-process core that the
 * fontd service wraps (see programs/fontd): register the faces you have,
 * then ask df_resolve() which one to use.
 *
 * It is filesystem-free: it operates on df_face* handles the caller has
 * already opened, and reads each face's weight/italic straight from the
 * font (OS/2 / head) -- "use the fonts as they are". Nearest-match
 * follows CSS-style weight distance; missing code points walk a fallback
 * chain. Synthetic bold/oblique are emitted as size-independent hints
 * the caller turns into a df_raster_req.
 */

#ifndef DOBFONT_RESOLVER_H
#define DOBFONT_RESOLVER_H

#include <dobfont/dobfont.h>

typedef struct df_fontset df_fontset;

/* Create / destroy a set. destroy does NOT close the faces -- their
 * lifetime (and that of their font blobs) belongs to the caller. */
df_fontset *df_fontset_create(void);
void        df_fontset_destroy(df_fontset *set);

/* Register a face under a family name. Its weight (100..900) and italic
 * flag are read from the face. path/face_index are stored verbatim so a
 * resolution result can point a separate rasterizer -- or a fontd client
 * -- at the underlying file. */
df_result df_fontset_add(df_fontset *set, const char *family,
                         df_face *face, const char *path, uint32_t face_index);

/* Ordered fallback family chain, tried (in order) for code points the
 * chosen family cannot render. Up to 8 families are kept. */
df_result df_fontset_set_fallback(df_fontset *set,
                                  const char *const *families, int count);

/* A resolution request. */
typedef struct
{
    const char *family;     /* desired family; NULL/"" = any */
    int         weight;     /* 100..900 (400 normal, 700 bold) */
    bool        italic;
    uint32_t    codepoint;  /* code point that must be present, 0 = ignore coverage */
} df_query;

/* The chosen face plus synthesis hints. slant is a shear factor (tan of
 * the angle); embolden_em is extra weight as a fraction of the em that
 * the caller scales by the pixel size. Both 0 when a designed face was
 * matched. */
typedef struct
{
    df_face    *face;
    const char *path;
    uint32_t    face_index;
    uint32_t    gid;          /* glyph id of codepoint in `face` (0 if codepoint==0) */
    float       slant;        /* synthetic oblique, 0 = upright/real italic */
    float       embolden_em;  /* synthetic bold per-em, 0 = real weight */
} df_resolved;

df_result df_resolve(const df_fontset *set, const df_query *q, df_resolved *out);

/* Fill a df_raster_req from a resolution result and a pixel size. */
void df_resolved_raster_req(const df_resolved *r, float px_size, df_raster_req *req);

/* ---- type=font manifest.dob parsing (string-only, no filesystem) ---- */
typedef struct { char file[128]; uint32_t index; } df_font_face_spec;

typedef struct
{
    char              name[64];
    char              family[64];
    df_font_face_spec faces[16];
    int               num_faces;
} df_font_manifest;

/* Parse a key=value manifest buffer:
 *   name=Inter
 *   family=Inter
 *   type=font
 *   faces=Inter-Regular.otf,Inter-Bold.otf,Inter-Italic.otf:0
 * Each face entry is "file[:index]". */
df_result df_font_manifest_parse(const char *text, uint32_t len,
                                 df_font_manifest *out);

#endif /* DOBFONT_RESOLVER_H */
