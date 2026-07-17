/* pagetest -- headless validator for the page-object engine.
 *
 * Builds a short-page document so it spans several sheets, drives the
 * engine, and proves: pagination into sheets, only the visible sheets are
 * rendered, the content is composited INTO each sheet's surface (ASCII
 * thumbnail), an incremental edit, and content<->window round-tripping.
 *
 *   pagetest /path/to/font.ttf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <DobFileSystem.h>
#include <dobdoc/dobdoc.h>
#include <dobfont/dobfont.h>
#include <dobfont/resolver.h>
#include <doblayout/doblayout.h>
#include <dobpage/dobpage.h>

#define CHUNK 65536

static bool read_all(const char *path, uint8_t **out, uint32_t *size)
{
    int fd = dobfs_Open(path, FS_READ); if (fd < 0) return false;
    uint8_t *buf = NULL; uint32_t len = 0, cap = 0;
    for (;;) {
        if (len + CHUNK > cap) { uint32_t nc = cap ? cap * 2 : CHUNK * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, nc); if (!nb) { free(buf); dobfs_Close(fd); return false; } buf = nb; cap = nc; }
        int n = dobfs_Read(fd, (char *)(buf + len), CHUNK);
        if (n < 0) { free(buf); dobfs_Close(fd); return false; }
        if (n == 0) break; len += (uint32_t)n;
    }
    dobfs_Close(fd); *out = buf; *size = len; return true;
}

static void thumb(const uint32_t *buf, int w, int h)
{
    const char ramp[] = " .:-=+*#%@";
    int scale = (w + 55) / 56; if (scale < 1) scale = 1;
    int cols = w / scale; if (cols > 100) cols = 100;
    int rows = h / scale; if (rows > 24) rows = 24;
    char line[110];
    for (int ry = 0; ry < rows; ry++) {
        int cc = cols < 109 ? cols : 109;
        for (int rx = 0; rx < cc; rx++) {
            uint32_t ink = 0, cnt = 0;
            for (int yy = 0; yy < scale; yy++) { int py = ry * scale + yy; if (py >= h) break;
                for (int xx = 0; xx < scale; xx++) { int px = rx * scale + xx; if (px >= w) break;
                    ink += 255 - ((buf[(size_t)py * w + px] >> 16) & 0xff); cnt++; } }
            uint32_t avg = cnt ? ink / cnt : 0;
            line[rx] = ramp[avg * 9 / 255];
        }
        line[cc] = 0; puts(line);
    }
}

int main(int argc, char **argv)
{
    if (argc < 1 || !argv[0] || !argv[0][0]) { printf("usage: pagetest <font-file>\n"); return 1; }
    uint8_t *fb; uint32_t fsz;
    if (!read_all(argv[0], &fb, &fsz)) { printf("pagetest: cannot read %s\n", argv[0]); return 1; }
    df_face *face; if (df_open(fb, fsz, 0, &face) != DF_OK) { printf("pagetest: bad font\n"); free(fb); return 1; }
    df_fontset *fonts = df_fontset_create(); df_fontset_add(fonts, "Doc", face, argv[0], 0);

    df_doc *d = df_doc_create();
    PageSetup ps; df_doc_get_page(d, &ps);
    ps.width = 4320; ps.height = 2880;                       /* 3 x 2 in -> many short sheets */
    ps.margin_left = ps.margin_right = 480; ps.margin_top = ps.margin_bottom = 360;
    df_doc_set_page(d, &ps);
    uint16_t fam = df_doc_family_intern(d, "Doc");
    CharFmt def; df_doc_get_default_char(d, &def); def.family_id = fam; def.size_twips = 240;
    df_doc_set_default_char(d, &def);

    uint32_t pos = 0;
    for (int i = 0; i < 6; i++) {
        char para[160];
        snprintf(para, sizeof para, "Paragrafo numero %d con abbastanza testo da andare a capo e riempire i fogli a sufficienza.", i + 1);
        df_doc_insert(d, pos, para, strlen(para)); pos += strlen(para);
        df_doc_insert(d, pos, "\n", 1); pos += 1;
    }

    dl_opts opt; opt.dpi = 96.0f;
    df_layout *L = df_layout_create(d, fonts, &opt);
    if (!L) { printf("pagetest: layout failed\n"); return 1; }

    dp_engine *e = dp_create(L);
    if (!e) { printf("pagetest: engine failed\n"); return 1; }
    int WW = 420, WH = 320;
    dp_set_viewport(e, WW, WH);

    printf("colonna alta %d px -> %u fogli (foglio %dx%d px), finestra %dx%d\n",
           (int)df_layout_content_height(L), dp_page_count(e), dp_page_w(e), dp_page_h(e), WW, WH);

    dp_view vis[4];
    int nv = dp_visible_pages(e, vis, 4);
    printf("\nscroll=0: %d fogli visibili\n", nv);
    for (int i = 0; i < nv; i++)
        printf("  foglio %d a finestra (%d,%d)\n", vis[i].page_index, vis[i].win_x, vis[i].win_y);
    if (nv > 0) { printf("contenuto composto NEL foglio %d:\n", vis[0].page_index); thumb(vis[0].buf, vis[0].w, vis[0].h); }

    /* scroll down: different sheets become live */
    dp_set_scroll(e, dp_page_h(e) + 16);
    nv = dp_visible_pages(e, vis, 4);
    printf("\nscroll=%d: %d fogli visibili (indici", dp_scroll(e), nv);
    for (int i = 0; i < nv; i++) printf(" %d", vis[i].page_index);
    printf(") -- prova del riciclo del pool\n");

    /* incremental edit on page 0: insert without newline */
    dp_set_scroll(e, 0);
    dp_visible_pages(e, vis, 4);
    df_doc_insert(d, 5, "XX", 2);
    dl_update u; df_layout_reflow(L, 5, 0, 2, &u);
    dp_notify_edit(e, &u);
    printf("\n[edit incrementale] height_delta=%d, paragrafi %u->%u -> percorso %s\n",
           (int)u.height_delta, u.para_count_old, u.para_count_new,
           (u.height_delta == 0.0f && u.para_count_old == u.para_count_new) ? "STRIP (solo la striscia)" : "ripagina");

    /* coordinate round-trip */
    dl_locus loc;
    if (df_layout_locate(L, 5, &loc)) {
        int wx, wy; dp_content_to_window(e, loc.x, (loc.y_top + loc.y_bottom) * 0.5f, &wx, &wy);
        float cx, cy; dp_window_to_content(e, wx, wy, &cx, &cy);
        printf("round-trip content->window->content: x %d->%d, y %d->%d\n",
               (int)loc.x, (int)cx, (int)((loc.y_top + loc.y_bottom) * 0.5f), (int)cy);
    }

    dp_destroy(e);
    df_layout_destroy(L);
    df_doc_destroy(d);
    df_fontset_destroy(fonts);
    df_close(face);
    free(fb);
    return 0;
}
