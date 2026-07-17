/* laytest -- headless validator for the persistent/incremental layout.
 *
 * Builds a multi-paragraph document, prints the content-space layout, then
 * proves incrementality: inserting a character into a middle paragraph
 * re-flows ONLY that paragraph (the ones below just translate by the height
 * delta), and inserting a newline splits one paragraph into two.
 *
 *   laytest /path/to/font.ttf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <DobFileSystem.h>
#include <dobdoc/dobdoc.h>
#include <dobfont/dobfont.h>
#include <dobfont/resolver.h>
#include <doblayout/doblayout.h>

#define CHUNK 65536

static bool read_all(const char *path, uint8_t **out, uint32_t *size)
{
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) return false;
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

static int iround(float v) { return (int)(v + 0.5f); }

static void dump(const df_layout *L, const char *tag)
{
    printf("--- %s: %u paragrafi, colonna %d x %d px ---\n", tag,
           df_layout_para_count(L), iround(df_layout_content_width(L)), iround(df_layout_content_height(L)));
    for (uint32_t i = 0; i < df_layout_para_count(L); i++) {
        const dl_para *p = df_layout_para(L, i);
        printf("  P%u byte[%u..%u) y=%d h=%d righe=%u\n", p->index,
               p->byte_start, p->byte_start + p->byte_len, iround(p->y), iround(p->height), p->line_count);
    }
}

int main(int argc, char **argv)
{
    if (argc < 1 || !argv[0] || !argv[0][0]) { printf("usage: laytest <font-file>\n"); return 1; }
    uint8_t *fb; uint32_t fsz;
    if (!read_all(argv[0], &fb, &fsz)) { printf("laytest: cannot read %s\n", argv[0]); return 1; }
    df_face *face;
    if (df_open(fb, fsz, 0, &face) != DF_OK) { printf("laytest: not a usable font\n"); free(fb); return 1; }

    df_fontset *fonts = df_fontset_create();
    df_fontset_add(fonts, "Doc", face, argv[0], 0);

    df_doc *d = df_doc_create();
    PageSetup ps; df_doc_get_page(d, &ps);
    ps.width = 4320; ps.margin_left = ps.margin_right = 720;   /* 3in wide -> forces wrapping */
    df_doc_set_page(d, &ps);
    uint16_t fam = df_doc_family_intern(d, "Doc");
    CharFmt def; df_doc_get_default_char(d, &def); def.family_id = fam; def.size_twips = 240;
    df_doc_set_default_char(d, &def);

    const char *p0 = "Primo paragrafo, abbastanza lungo da andare a capo su piu righe nella colonna stretta.";
    const char *p1 = "Secondo paragrafo che modificheremo.";
    const char *p2 = "Terzo paragrafo, resta sotto e deve solo traslare.";
    uint32_t pos = 0;
    df_doc_insert(d, pos, p0, strlen(p0)); pos += strlen(p0); df_doc_insert(d, pos, "\n", 1); pos += 1;
    uint32_t p1_start = pos;
    df_doc_insert(d, pos, p1, strlen(p1)); pos += strlen(p1); df_doc_insert(d, pos, "\n", 1); pos += 1;
    df_doc_insert(d, pos, p2, strlen(p2)); pos += strlen(p2);

    dl_opts opt; opt.dpi = 96.0f;
    df_layout *L = df_layout_create(d, fonts, &opt);
    if (!L) { printf("laytest: layout failed\n"); return 1; }
    dump(L, "INIZIALE");

    /* note paragraph 2's y before editing paragraph 1 */
    float p2_y_before = df_layout_para(L, 2)->y;
    float p1_h_before = df_layout_para(L, 1)->height;

    /* INCREMENTALE: inserisci una parola nel paragrafo 1 (nessun newline) */
    const char *ins = " EXTRA";
    df_doc_insert(d, p1_start + 5, ins, strlen(ins));
    dl_update u;
    df_layout_reflow(L, p1_start + 5, 0, strlen(ins), &u);
    printf("\n[reflow inserimento in P1] first_para=%u old=%u new=%u height_delta=%d dirty_y=[%d..%d]\n",
           u.first_para, u.para_count_old, u.para_count_new, iround(u.height_delta),
           iround(u.dirty_y0), iround(u.dirty_y1));

    float p2_y_after = df_layout_para(L, 2)->y;
    float p1_h_after = df_layout_para(L, 1)->height;
    printf("  P1 altezza: %d -> %d   P2 y: %d -> %d   (shift atteso == height_delta)\n",
           iround(p1_h_before), iround(p1_h_after), iround(p2_y_before), iround(p2_y_after));
    printf("  PROVA incrementale: P2 shift = %d, height_delta = %d -> %s\n",
           iround(p2_y_after - p2_y_before), iround(u.height_delta),
           (iround(p2_y_after - p2_y_before) == iround(u.height_delta)) ? "OK" : "DIFFORME");
    printf("  conteggio paragrafi invariato (%u) -> %s\n", df_layout_para_count(L),
           df_layout_para_count(L) == 3 ? "OK" : "NO");
    dump(L, "DOPO INSERIMENTO");

    /* SPLIT: inserisci un newline dentro il paragrafo 1 -> deve diventare 2 paragrafi */
    uint32_t before_split = df_layout_para_count(L);
    df_doc_insert(d, p1_start + 3, "\n", 1);
    df_layout_reflow(L, p1_start + 3, 0, 1, &u);
    printf("\n[reflow split con newline] paragrafi %u -> %u  (atteso +1) -> %s\n",
           before_split, df_layout_para_count(L),
           df_layout_para_count(L) == before_split + 1 ? "OK" : "NO");
    dump(L, "DOPO SPLIT");

    /* query: caret e hit-test */
    dl_locus loc;
    if (df_layout_locate(L, p1_start, &loc))
        printf("\nlocate(byte %u) -> P%u L%u x=%d y=[%d..%d]\n", p1_start,
               loc.para, loc.line, iround(loc.x), iround(loc.y_top), iround(loc.y_bottom));
    uint32_t hit = df_layout_hit(L, loc.x + 2.0f, (loc.y_top + loc.y_bottom) * 0.5f);
    printf("hit(x,y vicino al caret) -> byte %u\n", hit);

    df_layout_destroy(L);
    df_doc_destroy(d);
    df_fontset_destroy(fonts);
    df_close(face);
    free(fb);
    return 0;
}
