/* doctest -- headless validator for the DobWrite document model.
 *
 * Builds a small formatted document (a centered heading via a named
 * style, body text with one bold word), prints the *resolved* paragraph
 * and character formatting (so the style cascade is visible), then
 * serializes -> loads -> re-serializes and checks the bytes are
 * identical. With a path argument it also writes the .dobw file.
 *
 *   doctest                 build, dump, round-trip in memory
 *   doctest /out/test.dobw  ... and save the document to disk
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <DobFileSystem.h>
#include <dobdoc/dobdoc.h>

static int index_of(const char *hay, const char *needle)
{
    size_t hn = strlen(hay), nn = strlen(needle);
    if (nn == 0 || nn > hn) return -1;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(hay + i, needle, nn) == 0) return (int)i;
    return -1;
}

static const char *align_name(uint8_t a)
{
    switch (a) {
        case DD_ALIGN_CENTER:  return "center";
        case DD_ALIGN_RIGHT:   return "right";
        case DD_ALIGN_JUSTIFY: return "justify";
        default:               return "left";
    }
}

static bool write_all(const char *path, const uint8_t *data, uint32_t n)
{
    int fd = dobfs_Open(path, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0) return false;
    uint32_t off = 0;
    while (off < n) {
        int w = dobfs_Write(fd, (const char *)(data + off), n - off);
        if (w <= 0) { dobfs_Close(fd); return false; }
        off += (uint32_t)w;
    }
    dobfs_Close(fd);
    return true;
}

static void print_text(df_doc *d, uint32_t start, uint32_t len)
{
    char buf[256];
    uint32_t got = df_doc_get_text(d, start, len, buf, sizeof(buf) - 1);
    buf[got] = '\0';
    /* strip a trailing newline for tidy printing */
    if (got && buf[got - 1] == '\n') buf[got - 1] = '\0';
    printf("\"%s\"", buf);
}

static void dump(df_doc *d, const char *label)
{
    printf("---- %s ----\n", label);
    uint32_t len   = df_doc_length(d);
    uint32_t paras = df_doc_para_count(d);
    printf("length=%u bytes, paragraphs=%u\n", len, paras);

    for (uint32_t i = 0; i < paras; i++) {
        uint32_t s = 0, l = 0;
        df_doc_para_range(d, i, &s, &l);
        ParaFmt pf; df_doc_para_fmt_resolved(d, i, &pf);

        Style st; const char *sname = "?";
        if (df_doc_style_get(d, pf.style_id, &st) == DD_OK) sname = st.name;

        printf("  P%u [%u,%u) align=%-7s style=%-10s sp_before=%u sp_after=%u  ",
               i, s, s + l, align_name(pf.align), sname, pf.space_before, pf.space_after);
        print_text(d, s, l);
        printf("\n");
    }

    /* coalesce the text into runs of constant EFFECTIVE character format */
    printf("  effective character runs:\n");
    CharFmt prev; memset(&prev, 0, sizeof prev);
    bool have = false; uint32_t seg = 0;
    for (uint32_t pos = 0; ; pos = df_doc_next_cp(d, pos)) {
        if (pos >= len) {
            if (have)
                printf("    [%u,%u) %upt%s%s family=%s\n", seg, len,
                       prev.size_twips / 20, prev.bold ? " bold" : "",
                       prev.italic ? " italic" : "", df_doc_family_name(d, prev.family_id));
            break;
        }
        CharFmt cur; df_doc_char_fmt_at(d, pos, &cur);
        if (!have) { prev = cur; seg = pos; have = true; }
        else if (memcmp(&cur, &prev, sizeof cur) != 0) {
            printf("    [%u,%u) %upt%s%s family=%s\n", seg, pos,
                   prev.size_twips / 20, prev.bold ? " bold" : "",
                   prev.italic ? " italic" : "", df_doc_family_name(d, prev.family_id));
            prev = cur; seg = pos;
        }
    }
}

int main(int argc, char **argv)
{
    df_doc *d = df_doc_create();
    if (!d) { printf("doctest: out of memory\n"); return 1; }

    /* default body font: Inter 11pt */
    uint16_t inter = df_doc_family_intern(d, "Inter");
    CharFmt def; df_doc_get_default_char(d, &def);
    def.family_id = inter; def.size_twips = 220;
    df_doc_set_default_char(d, &def);

    /* a heading style: 20pt bold, with space around it (inherits Inter from Normal/default) */
    CharFmt hcf; memset(&hcf, 0, sizeof hcf);
    hcf.mask = DD_CF_SIZE | DD_CF_BOLD; hcf.size_twips = 400; hcf.bold = 1;
    ParaFmt hpf; memset(&hpf, 0, sizeof hpf);
    hpf.mask = DD_PF_SPACE_BEFORE | DD_PF_SPACE_AFTER; hpf.space_before = 240; hpf.space_after = 120;
    int h1 = df_doc_style_define(d, "Heading 1", 0 /*parent Normal*/, &hcf, &hpf);

    /* type the document */
    const char *t_head = "Titolo del documento";
    const char *t_body = "Un paragrafo di prova con una parola in grassetto e poi la fine.";
    uint32_t p = 0;
    df_doc_insert(d, p, t_head, (uint32_t)strlen(t_head)); p += (uint32_t)strlen(t_head);
    df_doc_insert(d, p, "\n", 1);                          p += 1;
    uint32_t body_start = p;
    df_doc_insert(d, p, t_body, (uint32_t)strlen(t_body)); p += (uint32_t)strlen(t_body);
    df_doc_insert(d, p, "\n", 1);                          p += 1;

    /* heading paragraph: apply the style and center it */
    df_doc_set_para_style(d, 0, 0, h1);
    ParaFmt cpf; memset(&cpf, 0, sizeof cpf); cpf.mask = DD_PF_ALIGN; cpf.align = DD_ALIGN_CENTER;
    df_doc_set_para_fmt(d, 0, 0, &cpf);

    /* bold the word "grassetto" in the body */
    int wi = index_of(t_body, "grassetto");
    if (wi >= 0) {
        CharFmt b; memset(&b, 0, sizeof b); b.mask = DD_CF_BOLD; b.bold = 1;
        df_doc_apply_char_fmt(d, body_start + (uint32_t)wi, (uint32_t)strlen("grassetto"), &b);
    }

    dump(d, "original");

    /* exercise undo: undo the bold, then redo it */
    printf("can_undo=%d  ", df_doc_can_undo(d));
    df_doc_undo(d);                 /* undoes the bold apply */
    printf("after undo bold -> word bold should be gone\n");
    df_doc_redo(d);                 /* redo it */

    /* round-trip: serialize -> load -> serialize, compare */
    uint8_t *buf = NULL; uint32_t sz = 0;
    if (df_doc_serialize(d, &buf, &sz) != DD_OK) { printf("doctest: serialize failed\n"); return 1; }
    printf("\nserialized: %u bytes\n", sz);

    df_doc *d2 = NULL;
    if (df_doc_load(buf, sz, &d2) != DD_OK) { printf("doctest: load failed\n"); free(buf); return 1; }
    dump(d2, "reloaded");

    uint8_t *buf2 = NULL; uint32_t sz2 = 0;
    df_doc_serialize(d2, &buf2, &sz2);
    bool same = (sz == sz2) && (memcmp(buf, buf2, sz) == 0);
    printf("\nround-trip identical: %s (%u vs %u bytes)\n", same ? "YES" : "NO", sz, sz2);

    if (argc > 0 && argv[0] && argv[0][0]) {
        if (write_all(argv[0], buf, sz)) printf("wrote %s\n", argv[0]);
        else                             printf("could not write %s\n", argv[0]);
    }

    free(buf); free(buf2);
    df_doc_destroy(d2);
    df_doc_destroy(d);
    return 0;
}
