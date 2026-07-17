/* fonttest -- headless validator for libdobfont.
 *
 * Loads a real TTF/OTF file, reports face info and metrics, maps a
 * code point to a glyph, rasterizes it at a pixel size and prints the
 * R8 coverage as ASCII art -- so the whole parse + raster pipeline can
 * be exercised from a console, before any dv/window plumbing exists.
 *
 *   fonttest <font.ttf> [char|0xHEX|U+HEX] [size]
 *
 * Following the MainDOB convention (see programs/editor), argv[0] is
 * the first *user* argument: the font path. Defaults: char 'A', 48 px.
 *
 * No libdobui, no graphics. Only the filesystem (to read the file) and
 * stdio (to print). Numbers are printed rounded to integers to avoid
 * depending on float formatting in the libc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <DobFileSystem.h>
#include <dobfont/dobfont.h>

#define READ_CHUNK 65536

static bool read_all(const char *path, uint8_t **out_data, uint32_t *out_size)
{
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) return false;

    uint8_t *buf = NULL; uint32_t len = 0, cap = 0;
    for (;;) {
        if (len + READ_CHUNK > cap) {
            uint32_t nc = cap ? cap * 2 : READ_CHUNK * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, nc);
            if (!nb) { free(buf); dobfs_Close(fd); return false; }
            buf = nb; cap = nc;
        }
        int n = dobfs_Read(fd, (char *)(buf + len), READ_CHUNK);
        if (n < 0) { free(buf); dobfs_Close(fd); return false; }
        if (n == 0) break;
        len += (uint32_t)n;
    }
    dobfs_Close(fd);
    *out_data = buf; *out_size = len;
    return true;
}

static uint32_t parse_cp(const char *s)
{
    if (!s || !s[0]) return 'A';
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (uint32_t)strtol(s + 2, NULL, 16);
    if ((s[0] == 'U' || s[0] == 'u') && s[1] == '+')  return (uint32_t)strtol(s + 2, NULL, 16);
    return (uint32_t)(unsigned char)s[0];
}

int main(int argc, char **argv)
{
    if (argc < 1 || !argv[0] || !argv[0][0]) {
        printf("uso: fonttest <font.ttf> [carattere|0xHEX] [dimensione]\n");
        return 1;
    }
    const char *path = argv[0];
    uint32_t cp   = (argc >= 2) ? parse_cp(argv[1]) : 'A';
    int      size = (argc >= 3) ? atoi(argv[2]) : 48;
    if (size <= 0) size = 48;

    uint8_t *data; uint32_t dsize;
    if (!read_all(path, &data, &dsize)) {
        printf("errore: impossibile leggere %s\n", path);
        return 1;
    }
    printf("file: %s (%u byte)\n", path, dsize);

    df_face *face;
    df_result r = df_open(data, dsize, 0, &face);
    if (r) { printf("errore: font non valido (codice %d)\n", (int)r); free(data); return 1; }

    printf("unitsPerEm = %u\n", df_units_per_em(face));
    printf("numGlyphs  = %u\n", df_num_glyphs(face));
    printf("outline    = %s\n",
           df_has_outlines(face) ? "si (TrueType/glyf)" : "no (CFF non ancora supportato)");

    df_vmetrics vm;
    if (df_face_vmetrics(face, (float)size, &vm) == DF_OK)
        printf("a %dpx: ascent=%d descent=%d interlinea=%d\n", size,
               (int)(vm.ascent + 0.5f), (int)(vm.descent - 0.5f),
               (int)(vm.line_height + 0.5f));

    uint32_t gid = df_map_codepoint(face, cp);
    printf("U+%04X -> glyph %u%s\n", cp, gid, gid ? "" : " (.notdef)");

    df_gmetrics gm;
    if (df_glyph_metrics(face, gid, (float)size, &gm) == DF_OK)
        printf("advance = %d px\n", (int)(gm.advance + 0.5f));

    if (!df_has_outlines(face)) {
        printf("(rasterizzazione saltata: questo .otf usa outline CFF)\n");
        df_close(face); free(data); return 0;
    }

    df_raster_req req;
    req.px_size = (float)size; req.embolden = 0.0f; req.slant = 0.0f; req.shift_x = 0.0f;

    df_bitmap bm;
    r = df_rasterize(face, gid, &req, &bm);
    if (r) { printf("errore raster (codice %d)\n", (int)r); df_close(face); free(data); return 1; }

    if (bm.w == 0 || bm.h == 0 || !bm.cover) {
        printf("(glifo vuoto, nessun inchiostro)\n");
    } else {
        printf("bitmap %dx%d  left=%d top=%d\n", bm.w, bm.h, bm.left, bm.top);
        static const char ramp[] = " .:-=+*#%@";   /* 10 livelli di copertura */
        char *line = (char *)malloc((size_t)bm.w + 1);
        if (line) {
            for (int y = 0; y < bm.h; y++) {
                for (int x = 0; x < bm.w; x++)
                    line[x] = ramp[(bm.cover[(size_t)y * bm.w + x] * 9) / 255];
                line[bm.w] = '\0';
                printf("%s\n", line);
            }
            free(line);
        }
    }

    df_free_bitmap(&bm);
    df_close(face);
    free(data);
    return 0;
}
